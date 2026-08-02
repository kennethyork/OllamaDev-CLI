// OllamaDev — a local AI coding agent with a crew.
// Copyright (C) 2026 Kenneth York
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option) any
// later version. See the LICENSE file, or <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: AGPL-3.0-or-later

// Dependency-free smoke tests. Anything that needs a live Ollama or a real
// coding CLI is skipped rather than failed, so this stays runnable in CI.
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>

#include "Backend.h"
#include "GitFlow.h"
#include "Config.h"
#include "Hooks.h"
#include "Json.h"
#include "Models.h"
#include "Parallel.h"
#include "Plugins.h"
#include "Rebase.h"
#include "Sandbox.h"
#include "SecScan.h"
#include "Session.h"
#include "Update.h"
#include "Tools.h"
#include "Tui.h"
#include "Crew.h"
#include "Vision.h"
#include "Workspaces.h"

using namespace odv;

static int passed = 0;
static int failed = 0;

static void check(bool ok, const QString& what) {
    QTextStream s(stdout);
    if (ok) {
        ++passed;
        s << "  ok    " << what << "\n";
    } else {
        ++failed;
        s << "  FAIL  " << what << "\n";
    }
    s.flush();
}

static void testJson() {
    // Cloud models fence their JSON even when told not to.
    const auto fenced = json::objectFrom("```json\n{\"clean\":true}\n```");
    check(fenced.value("clean").toBool(), "decodeLoose strips a code fence");

    const auto prosey = json::objectFrom("Sure! Here you go:\n{\"a\":{\"b\":2}}\nHope that helps.");
    check(json::at(prosey, "a.b").toInt() == 2, "decodeLoose survives surrounding prose");

    const auto flat = json::expandDotted(QJsonObject{{"crew.coderModel", "x"}});
    check(flat.value("crew").toObject().value("coderModel").toString() == "x",
          "expandDotted nests flat keys");
}

static void testModels() {
    check(Models::isCloud("gpt-oss:20b-cloud"), "isCloud recognises a -cloud tag");
    check(Models::isCloud("deepseek-v4-pro:cloud"), "isCloud recognises a :cloud tag");
    check(!Models::isCloud("qwen3.5:9b"), "isCloud rejects a local tag");
    check(Models::firstCloud({"qwen3.5:9b", "gpt-oss:20b-cloud"}) == "gpt-oss:20b-cloud",
          "firstCloud picks the cloud tag");
    check(Models::paramSizeB("8x7b") > 50, "paramSizeB expands an MoE tag");
}

static void testSecScan() {
    const auto f = SecScan::scanDiff("+AWS_SECRET=AKIAIOSFODNN7EXAMPLE\n-nothing\n");
    check(!f.isEmpty(), "scanDiff flags a credential on an added line");
    bool leaked = false;
    for (const auto& x : f)
        if (x.redacted.contains("IOSFODNN7")) leaked = true;
    check(!leaked, "findings never echo the full secret");

    const auto clean = SecScan::scanDiff("-AWS_SECRET=AKIAIOSFODNN7EXAMPLE\n");
    check(clean.isEmpty(), "scanDiff ignores removed lines");

    // `ollamadev scan` defaults to the current DIRECTORY, and scanFile() only ever
    // accepted a file — so the natural way to run the scanner examined NOTHING and
    // printed "clean". An all-clear from a scan that never happened is worse than
    // no scanner at all, so a tree scan is what the command actually needs.
    QTemporaryDir tree;
    QDir(tree.path()).mkpath(QStringLiteral("src"));
    QDir(tree.path()).mkpath(QStringLiteral("node_modules/junk"));
    const auto put = [](const QString& p, const char* text) {
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(text);
        f.close();
    };
    put(tree.path() + "/src/creds.py", "AWS_KEY = \"AKIAIOSFODNN7EXAMPLE\"\n");
    put(tree.path() + "/src/fine.py", "print('hello')\n");
    // A secret in node_modules is somebody else's problem, and scanning it is how a
    // scanner becomes slow enough to be switched off.
    put(tree.path() + "/node_modules/junk/leak.js", "const k = \"AKIAIOSFODNN7EXAMPLE\";\n");

    int files = 0;
    const auto found = SecScan::scanTree(tree.path(), &files);
    check(found.size() == 1, "scanTree walks a DIRECTORY (scanFile silently returned nothing)");
    check(found.first().file == QStringLiteral("src/creds.py"),
          "a tree finding says WHICH file — 'line 1' of what, otherwise?");
    check(files == 2, "node_modules is skipped, so the count is of files really read");

    int none = 0;
    SecScan::scanTree(tree.path() + QStringLiteral("/nope"), &none);
    check(none == 0, "a scan that reads nothing reports reading nothing — it must not say 'clean'");

    int one = 0;
    const auto direct = SecScan::scanTree(tree.path() + QStringLiteral("/src/creds.py"), &one);
    check(direct.size() == 1 && one == 1, "…and a single file is still a fine thing to scan");
}

static void testSandbox() {
    QTemporaryDir proj, sand, store;
    QDir(proj.path()).mkpath("src");
    QFile a(proj.path() + "/src/keep.txt");
    a.open(QIODevice::WriteOnly);
    a.write("one\n");
    a.close();

    QString err;
    check(Sandbox::copyTree(proj.path(), sand.path() + "/c1", &err),
          "copyTree mirrors a project");

    // The coder creates a file in a folder that does not exist in the project.
    QDir(sand.path() + "/c1").mkpath("src/deep/nested");
    QFile b(sand.path() + "/c1/src/deep/nested/new.txt");
    b.open(QIODevice::WriteOnly);
    b.write("hello\n");
    b.close();

    const Changeset cs = Sandbox::capture(proj.path(), sand.path() + "/c1", store.path());
    check(!cs.empty(), "capture sees the new file");
    check(cs.created.contains("src/deep/nested/new.txt"), "capture records the nested path");

    // `crew resume` reloads a finished coder's changeset from disk instead of
    // re-running the model, so load() must round-trip capture().
    const Changeset reloaded = Sandbox::load(store.path());
    check(reloaded.created == cs.created && reloaded.diff == cs.diff,
          "Sandbox::load round-trips a captured changeset");
    check(Sandbox::load("/nonexistent/store").empty(), "Sandbox::load of a missing store is empty");

    QStringList wrote;
    check(Sandbox::apply(store.path(), proj.path(), &wrote, &err), "apply lands the changeset");
    check(QFile::exists(proj.path() + "/src/deep/nested/new.txt"),
          "accept creates missing folders and the file");

    // A changeset must never be able to write outside the project.
    check(!Sandbox::apply("/nonexistent", proj.path(), nullptr, &err),
          "apply refuses a missing store");
}

static void testLimiter() {
    // A local model and a cloud model must not share an admission slot: the
    // whole point is that a cloud coder is not throttled by the local GPU.
    Limiter::instance().setLimit("ollama:local", 2);
    Limiter::instance().setLimit("ollama:cloud", 8);
    check(Limiter::instance().limit("ollama:local") == 2, "local backend is capped tight");
    check(Limiter::instance().limit("ollama:cloud") == 8, "cloud backend fans out wide");

    std::atomic_int concurrent{0};
    std::atomic_int peak{0};
    const auto r = parallelRun(
        6, [](int) { return QStringLiteral("ollama:local"); },
        [&](int) -> QJsonObject {
            const int now = ++concurrent;
            int want = peak.load();
            while (now > want && !peak.compare_exchange_weak(want, now)) {
            }
            QThread::msleep(40);
            --concurrent;
            return {};
        });
    check(r.size() == 6, "parallelRun returns every result");
    check(peak.load() <= 2, "parallelRun never exceeds the backend's limit");
}

static void testBackends() {
    check(Backends::all().first() == "ollama", "ollama is the default backend");
    check(Backends::all().contains("claude") && Backends::all().contains("codex") &&
              Backends::all().contains("gemini"),
          "the major coding CLIs are registered");
    auto claude = Backends::get("claude");
    check(claude && !claude->supportsNativeTools(),
          "coding CLIs run their own agent loop, so we never feed them tool schemas");
    auto ollama = Backends::get("ollama");
    check(ollama && ollama->supportsNativeTools(), "ollama does native function calling");
    check(ollama && ollama->concurrencyLimit("gpt-oss:20b-cloud") >
                        ollama->concurrencyLimit("qwen3.5:9b"),
          "a cloud model is allowed more concurrency than a local one");
}

// ---------------------------------------------------------------------------
// Regressions carried over from the PHP app. Both of these shipped in v0.9.78
// and are the reason this port exists in its current shape. They are asserted
// against the SOURCE, because both are bugs of omission — you cannot catch
// "somebody added a global pkill" by exercising a happy path.
// ---------------------------------------------------------------------------

// Code only, comments stripped. A guard that scanned comments too would fire on
// the comment that documents the very flag it is guarding against — which is
// exactly what happened the first time.
static QString readSource(const QString& rel) {
    QFile f(QStringLiteral(ODV_SOURCE_DIR) + "/" + rel);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QString code;
    for (const QString& line : QString::fromUtf8(f.readAll()).split('\n')) {
        const int c = line.indexOf(QStringLiteral("//"));
        code += (c >= 0 ? line.left(c) : line) + '\n';
    }
    return code;
}

static void testNoGlobalProcessNuking() {
    // PHP bug: Desktop/ollamadev-ade/index.php:71 ran PtyManager::cleanupStale()
    // on boot — `pkill -9 -f '__pty-daemon__'` plus `rm -rf ~/.ollamadev/terminals/*`.
    // Launching a SECOND instance therefore killed the FIRST one's live terminals.
    // Here every Pty is a child QObject of the widget that spawned it, so there is
    // nothing global to clean. Keep it that way.
    bool clean = true;
    // The ade/* files this guard also covers live in the desktop app, not in this
    // CLI-only tree, so they are checked in that repo. Here we scan the core files
    // the CLI actually ships.
    for (const QString& f : {QStringLiteral("core/Pty.cpp"),
                             // core/Terminals.cpp is THE file the bug lived in — it is the
                             // port of the multiplexer that ran the boot-time pkill. A guard
                             // that covered everything except the crime scene was theatre.
                             QStringLiteral("core/Terminals.cpp")}) {
        const QString src = readSource(f);
        if (src.isEmpty()) {
            clean = false;
            break;
        }
        // A process-wide kill by name, or wiping a shared terminals dir, would
        // reintroduce the second-instance bug.
        if (src.contains("pkill") || src.contains("killall") ||
            src.contains("cleanupStale")) {
            clean = false;
        }
    }
    check(clean, "no instance-global process kill (2nd ADE must not kill the 1st's terminals)");
}

static void testCliBackendIsSandboxed() {
    // A CLI backend is an agent in a subprocess: it does its OWN file edits, so
    // the thread-local tool root cannot confine it. Its working directory IS its
    // sandbox. Pointing it at the process cwd let a crew coder write straight
    // into the user's real project, bypassing the changeset, the auditor, the
    // secret gate and the overlap guard at once. Caught by a live mixed crew.
    const QString src = readSource(QStringLiteral("core/CliBackend.cpp"));
    check(src.contains("Tools::threadRoot()"),
          "CLI backends run in the coder's sandbox, not the process cwd");
    check(!src.contains("setWorkingDirectory(QDir::currentPath())"),
          "CLI backend never hardcodes the project root as its working directory");
}

static void testPerBackendModelRouting() {
    // A model tag belongs to exactly ONE backend. Two ways this broke a live
    // mixed crew, both caught by running one:
    //  1. inheriting the session's Ollama tag for a Claude/Codex coder — that
    //     CLI rejects the run with "model may not exist";
    //  2. `--coder-models tag,,` parsed with SkipEmptyParts collapsing to a
    //     one-element list, so the round-robin wrapped and handed coder 2 the
    //     Ollama tag anyway. These lists are POSITIONAL: entry i is coder i.
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("modelFor("),
          "each crew role resolves its model against its OWN backend");

    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(!cli.contains("v.split(',', Qt::SkipEmptyParts)"),
          "positional --coder-* lists keep empty slots (they mean 'backend default')");
}

static void testCliArgvIsCurrent() {
    // PHP bug: src/52-model-client.php:135 pinned flags that these CLIs removed,
    // so --backend claude|codex|qwen was broken in the shipping app. Assert we
    // never resurrect the dead flags.
    const QString src = readSource(QStringLiteral("core/CliBackend.cpp"));
    check(!src.isEmpty(), "CliBackend source is readable");
    check(!src.contains("--tools"), "claude: --tools was removed from the CLI");
    check(!src.contains("--ask-for-approval"),
          "codex: --ask-for-approval was removed from `exec`");
    check(!src.contains("--approval-mode plan") && !src.contains("--input-format"),
          "qwen: --approval-mode plan / --input-format do not exist");
    check(src.contains("--skip-trust"),
          "gemini: --skip-trust is required or yolo silently downgrades and hangs");
}

// `crew resume` must reload the saved plan and skip finished coders, never
// re-plan or re-run work that already landed.
static void testCrewResume() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("writePlan(") && crew.contains("plan.json"),
          "crew persists a durable plan.json so a run can be resumed");
    check(crew.contains("bool resuming") && crew.contains("opts.resumeRunId"),
          "run() branches on resumeRunId to reload the plan instead of re-planning");
    check(crew.contains("skipRun"),
          "resume skips coders that already finished (done/held)");
    check(crew.contains("replan") && crew.contains("doneTitles"),
          "resume re-plans only the leftover work, telling the Director what's done");
    check(crew.contains("reloaded") && crew.contains("audit.clean = true"),
          "finished coders land from disk and are never re-audited on resume");
    check(crew.contains("doRoute") && crew.contains("plan.value(\"route\")"),
          "brain settings (route/debate/dedupe) survive a resume via the plan");
    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(cli.contains("cmdCrewResume") && cli.contains("\"resume\""),
          "CLI wires `crew resume`");
    check(cli.contains("--replay"), "CLI exposes --replay (opt out of the default re-plan)");
}

// --amplify N: N Director plans (keep the modal one) + an N-reviewer audit panel
// that needs a STRICT majority. And a crew pack must actually reach a run — the
// pack keys were written to disk and then read by nobody, so `--amplify 3` in a
// saved pack silently did nothing.
static void testCrewAmplify() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("planOnce") && crew.contains("amplify <= 1"),
          "the Director draws one plan per sample and short-circuits at amplify=1");
    check(crew.contains("freq[c.size()]"),
          "self-consistency keeps the plan whose subtask COUNT is the mode");
    check(crew.contains("reviewOnce") && crew.contains("pass % 2 == 1"),
          "the audit panel alternates neutral and skeptic seats");
    check(crew.contains("clean * 2 > amplify"),
          "a changeset lands only on a STRICT majority of clean votes (2-2 holds)");
    check(crew.contains("plan.value(\"amplify\")"),
          "amplify rides in plan.json, so a resumed run re-plans with the same rigour");

    const QString cli = readSource(QStringLiteral("cli/main.cpp"));
    check(cli.contains("o.amplify = num(\"--amplify\", \"amplify\", 1)"),
          "CLI: --amplify wins over the pack, the pack over the default");
    check(cli.contains("CrewPacks::load(packName)"),
          "crew --pack loads a saved team as the base (it used to load nothing)");
    // Both must sit in the value-taking list or positionals() reads their argument
    // as the task. Matched against the function body rather than a fixed run of
    // spaces, so reformatting the list cannot fail this for the wrong reason.
    const int listStart = cli.indexOf(QStringLiteral("flagsTakingValue()"));
    const int listEnd = cli.indexOf(QStringLiteral("return v;"), listStart);
    const QString valueFlags = listStart < 0 ? QString() : cli.mid(listStart, listEnd - listStart);
    check(valueFlags.contains(QStringLiteral("\"--amplify\"")) &&
              valueFlags.contains(QStringLiteral("\"--pack\"")),
          "--amplify/--pack take a value, so they never swallow the task word");

    // The real bug this guards: a pack key that no run reads. If someone adds a
    // key to `crew pack save`, it has to be consumed in cmdCrew too.
    const int save = cli.indexOf(QStringLiteral("int cmdCrewPack"));
    const QString packSave = save < 0 ? QString() : cli.mid(save, 3000);
    for (const QString& key : {QStringLiteral("focus"), QStringLiteral("coderModel"),
                               QStringLiteral("amplify"), QStringLiteral("land"),
                               QStringLiteral("route")}) {
        check(packSave.contains(QStringLiteral("\"%1\"").arg(key)) &&
                  cli.contains(QStringLiteral("\"%1\"").arg(key)),
              QStringLiteral("pack key '%1' is both saved and read back").arg(key));
    }
}

// The agent tool suite, driven for real: a throwaway git repo, actual commits,
// an actual background job. These are the tools a model reaches for most, so they
// are tested by BEHAVIOUR, not by grepping the source.
static void testAgentTools() {
    Tools::registerAll();
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(false);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        check(false, "temp dir for the tool tests");
        return;
    }
    const QString root = tmp.path();

    // A real repo, made with real git. If git is missing there is nothing to test.
    auto git = [&](const QStringList& a) {
        QProcess p;
        p.setWorkingDirectory(root);
        p.start(QStringLiteral("git"), a);
        return p.waitForFinished(10000) && p.exitCode() == 0;
    };
    if (!git({QStringLiteral("init"), QStringLiteral("-q")})) {
        check(true, "git is not installed — skipping the git tool tests");
        return;
    }
    git({QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@t.t")});
    git({QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("t")});

    // Every tool resolves against the thread root, so this IS the sandbox boundary.
    Tools::setThreadRoot(root);

    QFile f(root + QStringLiteral("/hello.txt"));
    f.open(QIODevice::WriteOnly);
    f.write("hi\n");
    f.close();

    ToolResult r = Tools::run(QStringLiteral("git_status"), {});
    check(r.ok && r.output.contains(QStringLiteral("hello.txt")), "git_status sees the new file");

    r = Tools::run(QStringLiteral("git_add"), QJsonObject{{"all", true}});
    check(r.ok, "git_add stages everything");

    // A multi-line message goes over stdin, so a newline in it can never be argv.
    r = Tools::run(QStringLiteral("git_commit"),
                   QJsonObject{{"message", QStringLiteral("first: add hello\n\nwith a body")}});
    check(r.ok, "git_commit commits the staged tree");

    r = Tools::run(QStringLiteral("git_log"), QJsonObject{{"n", 5}});
    check(r.ok && r.output.contains(QStringLiteral("first: add hello")), "git_log lists the commit");

    // The PHP original interpolated `n` into a shell string unescaped. If that ever
    // comes back, this injects a command that would create a file in the repo.
    Tools::run(QStringLiteral("git_log"),
               QJsonObject{{"n", QStringLiteral("5; touch pwned")}});
    check(!QFileInfo(root + QStringLiteral("/pwned")).exists(),
          "git_log cannot be used to inject a shell command");

    r = Tools::run(QStringLiteral("git_branch"), {});
    check(r.ok, "git_branch lists branches");

    r = Tools::run(QStringLiteral("git_show"), QJsonObject{{"stat", true}});
    check(r.ok && r.output.contains(QStringLiteral("hello.txt")), "git_show shows HEAD");

    // A crew coder lives under an explicit thread root. Pushing from there would
    // send un-landed sandbox work to the real remote, so it must refuse.
    r = Tools::run(QStringLiteral("git_push"), {});
    check(!r.ok && r.error.contains(QStringLiteral("sandbox")),
          "git_push REFUSES to push out of a sandbox");

    // A failing git command must come back as a FAILURE, not as success carrying
    // the error text — that was the bug in every PHP git tool.
    r = Tools::run(QStringLiteral("git_checkout"), QJsonObject{{"branch", QStringLiteral("nope")}});
    check(!r.ok, "a failed git command reports failure, not success-with-error-text");

    // ---- background shells ----
    r = Tools::run(QStringLiteral("bg"),
                   QJsonObject{{"command", QStringLiteral("echo alpha; sleep 1; echo omega")}});
    check(r.ok && r.output.contains(QStringLiteral("bg_")), "bg starts a job and returns at once");
    QString id;
    for (const QString& w : r.output.split(QRegularExpression(QStringLiteral("[\\s(]")))) {
        if (w.startsWith(QStringLiteral("bg_"))) {
            id = w;
            break;
        }
    }
    check(!id.isEmpty(), "bg hands back a job id");

    r = Tools::run(QStringLiteral("wait_bg"), QJsonObject{{"bg_id", id}, {"seconds", 20}});
    check(r.ok && r.output.contains(QStringLiteral("finished")), "wait_bg blocks until the job ends");

    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", id}});
    check(r.ok && r.output.contains(QStringLiteral("alpha")) &&
              r.output.contains(QStringLiteral("omega")),
          "bash_output returns the job's output");
    // The cursor: a second read shows only what is NEW, and the job is done, so
    // that is nothing at all.
    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", id}});
    check(r.ok && r.output.contains(QStringLiteral("no new output")),
          "bash_output only ever shows what you have not already seen");

    r = Tools::run(QStringLiteral("bash_output"), QJsonObject{{"bg_id", QStringLiteral("../../etc/passwd")}});
    check(!r.ok, "a job id cannot traverse out of the bg directory");

    // ---- calc ----
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("2 + 3 * 4")}});
    check(r.ok && r.output == QStringLiteral("14"), "calc respects precedence");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("(1 + 2) ^ 3")}});
    check(r.ok && r.output == QStringLiteral("27"), "calc does parens and powers");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("-4 + 10")}});
    check(r.ok && r.output == QStringLiteral("6"), "calc does unary minus");
    // Both of these KILLED the PHP process: eval() raised a fatal Error that its
    // catch(Exception) never saw.
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("1/0")}});
    check(!r.ok && r.error.contains(QStringLiteral("zero")), "calc survives division by zero");
    r = Tools::run(QStringLiteral("calc"), QJsonObject{{"expr", QStringLiteral("1 +")}});
    check(!r.ok, "calc survives a malformed expression");

    // ---- ask_user ----
    // Non-interactive: there is nobody to answer, so it must return AT ONCE with a
    // usable instruction rather than blocking the run forever.
    r = Tools::run(QStringLiteral("ask_user"),
                   QJsonObject{{"question", QStringLiteral("which one?")}});
    check(r.ok && r.output.contains(QStringLiteral("not interactive")),
          "ask_user never blocks a run that has nobody to ask");

    Tools::setThreadRoot(QString());
}

// Vision: an @image token becomes base64 on the message and LEAVES the prompt.
// Every surface that takes typed text must go through Vision::attach — the
// one-shot path did not, and silently sent the model the literal path instead.
static void testVision() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        check(false, "temp dir for the vision test");
        return;
    }
    // A 1x1 PNG. Real bytes, so encodeBase64 has something true to do.
    static const unsigned char png[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f,
        0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00,
        0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49,
        0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const QString shot = tmp.path() + QStringLiteral("/shot.png");
    QFile f(shot);
    f.open(QIODevice::WriteOnly);
    f.write(reinterpret_cast<const char*>(png), sizeof(png));
    f.close();

    ChatMessage m;
    int attached = 0;
    QString cleaned = Vision::attach(m, QStringLiteral("what is wrong with @") + shot, &attached);
    check(attached == 1 && m.images.size() == 1, "an @image token attaches the image");
    check(m.images.value(0).startsWith(QStringLiteral("iVBORw0KGgo")),
          "the image is attached as base64 (a PNG header)");
    check(!cleaned.contains(shot), "the path is taken back OUT of the prompt the model reads");
    check(cleaned.contains(QStringLiteral("what is wrong with")), "the rest of the prompt survives");

    // /image is the other spelling, and a bare one still needs an instruction.
    ChatMessage m2;
    cleaned = Vision::attach(m2, QStringLiteral("/image ") + shot, &attached);
    check(m2.images.size() == 1 && !cleaned.isEmpty(),
          "/image attaches, and a bare /image still asks the model something");

    // A non-image @token is a file mention, NOT a picture — leave it alone.
    ChatMessage m3;
    const QString notes = tmp.path() + QStringLiteral("/notes.txt");
    QFile n(notes);
    n.open(QIODevice::WriteOnly);
    n.write("hi");
    n.close();
    cleaned = Vision::attach(m3, QStringLiteral("read @") + notes, &attached);
    check(m3.images.isEmpty() && cleaned.contains(notes),
          "a non-image @mention is left for the file-mention path");
}

// Live steering. Crew::steer wrote steer.jsonl and NOTHING read it back — the
// board's steer box, the CLI's `crew steer`, and the desktop all wrote into a
// file no coder ever opened. The fix is a consumer in the coder loop, so the
// guard is: the writer and the reader must both exist, and the reader must run
// BETWEEN iterations (never mid-tool, or a coder gets interrupted between
// deciding to write a file and writing it).
static void testCrewSteer() {
    const QString crew = readSource(QStringLiteral("core/Crew.cpp"));
    check(crew.contains("drainSteer"), "the coder loop actually READS steer.jsonl");
    check(crew.contains("steerSeen") && crew.contains("f.seek(steerSeen)"),
          "each coder remembers how far it read, so a steer is delivered exactly once");
    check(crew.contains("target == 0 || target == n"),
          "a coder takes messages addressed to it, or to the whole crew (target 0)");
    check(crew.contains("sink, cancel, drainSteer"),
          "the drain is wired into Agent::loop as the interject hook");

    const QString agent = readSource(QStringLiteral("core/Agent.cpp"));
    check(agent.contains("const Interject& interject") && agent.contains("interject()"),
          "Agent::loop takes a word from the human between iterations");
    check(agent.contains("sink.onTool"),
          "the agent reports each tool ACTION, which is what a live pane displays");

    // coderLog is the pane's feed: it must tail by offset, and it must survive a
    // log that shrank (a new run reusing the id) rather than slicing past the end.
    QTemporaryDir tmp;
    const QString runId = QStringLiteral("crew_testlog");
    const QString dir = Config::crewDir() + QStringLiteral("/") + runId;
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/coder-1.log");
    {
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        f.write("hello");
        f.close();
    }
    qint64 size = 0;
    QString chunk = Crew::coderLog(runId, 1, 0, &size);
    check(chunk == QStringLiteral("hello") && size == 5, "coderLog reads from the start");
    chunk = Crew::coderLog(runId, 1, size, &size);
    check(chunk.isEmpty(), "coderLog returns nothing new when nothing was written");
    {
        QFile f(path);
        f.open(QIODevice::Append);
        f.write(" world");
        f.close();
    }
    chunk = Crew::coderLog(runId, 1, 5, &size);
    check(chunk == QStringLiteral(" world"), "coderLog returns only the DELTA");
    chunk = Crew::coderLog(runId, 1, 9999, &size);
    check(chunk == QStringLiteral("hello world"),
          "an offset past the end restarts rather than slicing past it");
    check(Crew::coderLog(QStringLiteral("nope"), 1, 0, &size).isEmpty(),
          "coderLog of a run that never started is empty, not a crash");
    QDir(dir).removeRecursively();
}

// Steering only reaches a coder that is still working: the drain above runs
// inside the coder loop, and it only takes lines addressed to its own number or
// to 0. Crew::steer used to append regardless and return true, so talking to a
// finished run — or to a coder that never existed — printed "✓ sent" and put the
// message in a file nothing would ever read again.
static void testCrewSteerRefusesTheUnreachable() {
    const QString runId = QStringLiteral("crew_teststeer");
    const QString dir = Config::crewDir() + QStringLiteral("/") + runId;
    const QString boardPath = Config::crewDir() + QStringLiteral("/current.json");

    // current.json is the live board — the developer's real one. Put it back.
    QByteArray saved;
    bool had = false;
    {
        QFile f(boardPath);
        if (f.open(QIODevice::ReadOnly)) {
            saved = f.readAll();
            had = true;
        }
    }
    QDir().mkpath(dir);

    const auto setBoard = [&](bool active, const QString& state1, const QString& state2) {
        QJsonArray subs{QJsonObject{{"n", 1}, {"state", state1}, {"title", "one"}},
                        QJsonObject{{"n", 2}, {"state", state2}, {"title", "two"}}};
        const QJsonObject o{{"runId", runId}, {"active", active}, {"subtasks", subs},
                            {"task", "test"}};
        QFile f(boardPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(o).toJson());
    };

    QString err;
    setBoard(true, QStringLiteral("doing"), QStringLiteral("done"));
    check(Crew::steer(1, QStringLiteral("carry on"), &err), "steer reaches a coder that is working");
    check(!Crew::steer(2, QStringLiteral("too late"), &err) && err.contains(QStringLiteral("done")),
          "…and refuses one that has already finished, saying so");
    check(!Crew::steer(99, QStringLiteral("nobody"), &err) && err.contains(QStringLiteral("#1–#2")),
          "…and refuses a coder number this run never had, naming the range");
    check(!Crew::steer(1, QStringLiteral("   "), &err),
          "…and refuses an empty message, which the drain would drop anyway");
    check(Crew::steer(0, QStringLiteral("everyone")), "a broadcast reaches the crew while one works");

    // Nobody left to hear it.
    setBoard(true, QStringLiteral("done"), QStringLiteral("held"));
    check(!Crew::steer(0, QStringLiteral("hello?"), &err),
          "a broadcast with every coder stopped is refused, not silently filed");

    setBoard(false, QStringLiteral("doing"), QStringLiteral("doing"));
    check(!Crew::steer(1, QStringLiteral("hello?"), &err) &&
              err.contains(QStringLiteral("finished")),
          "a run that is over cannot be steered at all");

    // Only the lines that were accepted should have been written.
    QFile sf(dir + QStringLiteral("/steer.jsonl"));
    int lines = 0;
    if (sf.open(QIODevice::ReadOnly))
        for (const QByteArray& l : sf.readAll().split('\n'))
            if (!l.trimmed().isEmpty()) ++lines;
    check(lines == 2, "exactly the two accepted messages were written — the refusals wrote nothing");

    QDir(dir).removeRecursively();
    QFile::remove(boardPath);
    if (had) {
        QFile f(boardPath);
        if (f.open(QIODevice::WriteOnly)) f.write(saved);
    }
}

// Workspaces read and write the SAME ~/.ollamadev/workspaces.json the PHP app
// uses, which means the two can be run side by side during the port. The blob the
// desktop stores in `state` (canvas layout, terminal scrollback — tens of KB of
// it) is opaque to core, and a CLI `ws add` must never wipe it.
//
// HOME is redirected at the top so this never touches the real file.
static void testWorkspaces() {
    const QByteArray realHome = qgetenv("HOME");
    QTemporaryDir home;
    if (!home.isValid()) {
        check(false, "temp HOME for the workspace test");
        return;
    }
    qputenv("HOME", home.path().toUtf8());

    QTemporaryDir projA, projB;
    const Workspace a = Workspaces::add(projA.path(), QStringLiteral("alpha"));
    Workspaces::add(projB.path(), QStringLiteral("beta"));
    check(a.id.startsWith(QStringLiteral("ws_")), "a workspace id is derived from its path");
    check(Workspaces::all().size() == 2, "two workspaces are tracked");
    check(Workspaces::activeId() != a.id, "the most recently added one is active");

    // The desktop saves its canvas here. A later `ws add` on the same folder must
    // not clobber it — that would silently lose the user's window layout.
    const QJsonObject layout{{"panes", 3}, {"zoom", QStringLiteral("100%")}};
    check(Workspaces::saveState(a.id, layout), "the desktop can save its layout");
    Workspaces::add(projA.path(), QStringLiteral("alpha-renamed"));
    Workspace back;
    check(Workspaces::find(QStringLiteral("alpha-renamed"), &back), "re-adding renames in place");
    check(Workspaces::all().size() == 2, "re-adding the same path does NOT duplicate it");
    check(back.state == layout, "re-adding preserves the desktop's saved layout");

    check(Workspaces::find(QStringLiteral("BETA"), &back), "a workspace is found by name, any case");
    check(Workspaces::find(projB.path(), &back), "…and by path");
    check(Workspaces::find(back.id, &back), "…and by id");

    check(Workspaces::open(QStringLiteral("alpha-renamed")) == QFileInfo(projA.path()).canonicalFilePath(),
          "open prints the path (the shell does the cd — a child cannot chdir its parent)");
    check(Workspaces::activeId() == a.id, "open makes it active");

    // Removing the ACTIVE workspace must not leave `active` dangling.
    check(Workspaces::remove(QStringLiteral("alpha-renamed")), "remove works");
    check(Workspaces::all().size() == 1, "…and it is gone");
    check(!Workspaces::activeId().isEmpty() && Workspaces::activeId() != a.id,
          "removing the active workspace hands active to a survivor");
    check(!Workspaces::remove(QStringLiteral("nope")), "removing an unknown workspace fails cleanly");

    qputenv("HOME", realHome);
}

// Plugins. A plugin contributes skills, commands, hooks and MCP servers — the
// extension points that already exist. It cannot run code of its own.
//
// The bug this test exists for: hooks contributed by a plugin must NEVER reach
// Hooks::listFor(), because that is the EDITING view — `hooks add` reads it,
// modifies it, and writes the result back to the user's config. A plugin hook
// leaking in there gets baked permanently into their config file, where it then
// survives the plugin being disabled or even uninstalled. That happened.
static void testPlugins() {
    const QByteArray realHome = qgetenv("HOME");
    QTemporaryDir home, src, proj;
    if (!home.isValid() || !src.isValid()) {
        check(false, "temp dirs for the plugin test");
        return;
    }
    qputenv("HOME", home.path().toUtf8());

    // A plugin on disk: a manifest, a skill, and a hook that leaves a trace.
    const QString flag = home.path() + QStringLiteral("/hook-fired");
    QDir(src.path()).mkpath(QStringLiteral("skills/tidy"));
    QDir(src.path()).mkpath(QStringLiteral("commands"));
    const QString manifest =
        QStringLiteral(
            R"({"name":"tidy","version":"1.0","description":"d",
                "hooks":[{"event":"PostToolUse","matcher":"write","command":"touch '%1'"}],
                "mcp":{"tidy-ls":{"command":"python"}}})")
            .arg(flag);
    QFile mf(src.path() + QStringLiteral("/plugin.json"));
    mf.open(QIODevice::WriteOnly);
    mf.write(manifest.toUtf8());
    mf.close();
    QFile sk(src.path() + QStringLiteral("/skills/tidy/SKILL.md"));
    sk.open(QIODevice::WriteOnly);
    sk.write("---\nname: tidy\ndescription: how we format\n---\nbody\n");
    sk.close();

    QString err, name;
    check(Plugins::install(src.path(), &err, &name) && name == QStringLiteral("tidy"),
          "a plugin installs from a local directory");

    Plugin p;
    check(Plugins::get(QStringLiteral("tidy"), &p) && !p.enabled,
          "an installed plugin is DISABLED until the user says otherwise");
    check(p.hooks.size() == 1 && p.hasSkills && !p.mcp.isEmpty(),
          "the manifest's contributions are read");
    check(p.capabilities().contains(QStringLiteral("HOOK")),
          "capabilities() names the hooks loudly — that is what the consent prompt shows");

    // Disabled: contributes nothing.
    check(Plugins::hooksFor(QStringLiteral("PostToolUse")).isEmpty(),
          "a DISABLED plugin contributes no hooks");
    check(Plugins::skillDirs().isEmpty(), "a disabled plugin contributes no skills");
    check(Plugins::mcpServers().isEmpty(), "a disabled plugin contributes no MCP servers");

    check(Plugins::setEnabled(QStringLiteral("tidy"), true, &err), "the user enables it");
    check(Plugins::hooksFor(QStringLiteral("PostToolUse")).size() == 1,
          "an ENABLED plugin's hook is live");
    check(Plugins::skillDirs().size() == 1, "…and its skills are discoverable");
    check(Plugins::mcpServers().size() == 1, "…and its MCP servers are connected");

    // THE REGRESSION. listFor() is the editing view; a plugin hook in there gets
    // written into the user's own config by the next `hooks add`.
    check(Hooks::listFor(QStringLiteral("PostToolUse")).isEmpty(),
          "a plugin hook NEVER appears in the config editing view (it would be persisted)");

    // …but it really does fire, through the execution path.
    Tools::registerAll();
    Permission::setMode(PermMode::Auto);
    Permission::setInteractive(false);
    Tools::setThreadRoot(proj.path());
    Tools::run(QStringLiteral("write"), QJsonObject{{"file_path", QStringLiteral("x.txt")},
                                                    {"content", QStringLiteral("hi")}});
    check(QFileInfo(flag).exists(), "an enabled plugin's hook actually FIRES on a tool call");

    QFile::remove(flag);
    check(Plugins::setEnabled(QStringLiteral("tidy"), false, &err), "the user disables it");
    Tools::run(QStringLiteral("write"), QJsonObject{{"file_path", QStringLiteral("y.txt")},
                                                    {"content", QStringLiteral("hi")}});
    check(!QFileInfo(flag).exists(), "a DISABLED plugin's hook does NOT fire");

    check(Plugins::remove(QStringLiteral("tidy"), &err) && Plugins::all().isEmpty(),
          "a plugin uninstalls cleanly");

    // The PHP `plugin remove` passed its argument straight to unlink(), so
    // "../../something" was live. A name is a directory name, nothing else.
    check(!Plugins::remove(QStringLiteral("../../etc/passwd"), &err),
          "a plugin name cannot traverse out of the plugins directory");

    Tools::setThreadRoot(QString());
    qputenv("HOME", realHome);
}

// export/import must survive a session that USED TOOLS.
//
// The PHP export hand-rolled {id, messages, model} and replayed only role+content
// on import, so tool_calls were dropped: the assistant turns still said "I called
// edit()" and the tool results they correlated to were gone. A model reading that
// transcript is being lied to. Here the export IS the session's own on-disk JSON,
// and import decodes it through the same codec the app uses.
static void testExportImport() {
    QTemporaryDir proj;
    if (!proj.isValid()) {
        check(false, "temp project for the export test");
        return;
    }
    const QString cwd = QDir::currentPath();
    QDir::setCurrent(proj.path());  // sessions live under the PROJECT's .ollamadev

    Session s = Session::create(proj.path());
    ChatMessage user;
    user.role = QStringLiteral("user");
    user.content = QStringLiteral("fix the parser");
    ChatMessage asst;
    asst.role = QStringLiteral("assistant");
    asst.content = QStringLiteral("editing it now");
    asst.toolCalls = QJsonArray{QJsonObject{
        {"id", "call_1"},
        {"function", QJsonObject{{"name", "edit"}, {"arguments", "{\"file_path\":\"p.cpp\"}"}}}}};
    ChatMessage tool;
    tool.role = QStringLiteral("tool");
    tool.toolCallId = QStringLiteral("call_1");
    tool.toolName = QStringLiteral("edit");
    tool.content = QStringLiteral("edited p.cpp");
    s.messages() << user << asst << tool;
    s.setModel(QStringLiteral("qwen3.5:9b"));
    s.save();

    const QJsonObject bundle = Session::exportOne(s.id());
    check(!bundle.isEmpty() && bundle.value("messages").toArray().size() == 3,
          "export carries every message");

    QString err;
    const QString newId = Session::importOne(bundle, proj.path(), &err);
    check(!newId.isEmpty() && newId != s.id(),
          "import mints a NEW id — it can never clobber a session you have");

    auto back = Session::load(newId);
    check(back.has_value() && back->messages().size() == 3, "…and every message came back");
    if (back.has_value()) {
        const ChatMessage& a = back->messages().at(1);
        const ChatMessage& t = back->messages().at(2);
        check(!a.toolCalls.isEmpty(), "THE TOOL CALL SURVIVES the round trip (PHP dropped it)");
        check(t.toolCallId == QStringLiteral("call_1") && t.toolName == QStringLiteral("edit"),
              "…and the tool result still correlates to the call that made it");
        check(back->model() == QStringLiteral("qwen3.5:9b"), "the model survives too");
    }

    QDir::setCurrent(cwd);
}

// Overwriting the running binary is the one operation you do not get to be casual
// about: if it goes wrong the user has no working tool left to fix it with. These
// are asserted against the source because they are properties of the FAILURE
// paths, which a happy-path download would never exercise.
static void testUpdateSafety() {
    const QString src = readSource(QStringLiteral("core/Update.cpp"));
    check(src.contains("applicationFilePath"),
          "update resolves the running binary, never argv[0] (a symlink names something else)");
    check(src.contains("canonicalFilePath"), "…and canonicalises it");
    check(src.contains("blob.size() != info.assetSize"),
          "a short download is refused — a truncated binary is a brick");
    check(src.contains(".bak") && src.contains("QFile::rename(backup, info.target)"),
          "the old binary is kept and PUT BACK if the swap fails");
    check(src.contains("info.target + QStringLiteral(\".new\")"),
          "the new binary is staged NEXT TO the target — a rename across filesystems fails");
    check(src.contains("isWritable"),
          "it checks it can write BEFORE downloading 40MB into a root-owned directory");
    // The PHP fell back to assets[0] when it found no asset for this platform,
    // which installs another architecture's binary.
    check(src.contains("has no asset for this machine"),
          "no asset for THIS os/arch is an error, never a fallback to some other one");

    QString err;
    UpdateInfo empty;
    check(!Update::install(empty, &err), "install refuses when there is nothing to install");
}

// ACP: JSON-RPC over stdio, the protocol an editor drives us with. The handshake
// and session creation touch no model, so they are testable for real — we spawn
// the actual binary and speak the actual protocol to it.
static void testAcp() {
    const QString cli = QCoreApplication::applicationDirPath() + QStringLiteral("/../cli/ollamadev");
    if (!QFileInfo(cli).isExecutable()) {
        check(true, "ollamadev binary not built here — skipping the ACP protocol test");
        return;
    }

    QProcess p;
    p.start(cli, {QStringLiteral("acp")});
    if (!p.waitForStarted(5000)) {
        check(false, "the acp server starts");
        return;
    }

    const auto rpc = [&p](const QByteArray& line) -> QJsonObject {
        p.write(line + "\n");
        p.waitForBytesWritten(2000);
        // One frame per line. A server that ever printed anything else on stdout
        // would desynchronise a real editor's parser, so this is load-bearing.
        while (p.canReadLine() || p.waitForReadyRead(10000)) {
            const QByteArray raw = p.readLine().trimmed();
            if (raw.isEmpty()) continue;
            return QJsonDocument::fromJson(raw).object();
        }
        return {};
    };

    const QJsonObject init = rpc(
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1}})");
    const QJsonObject res = init.value("result").toObject();
    check(init.value("id").toInt() == 1 && res.value("protocolVersion").toInt() == 1,
          "initialize answers with the protocol version");
    check(res.value("agentInfo").toObject().value("name").toString() == QLatin1String("ollamadev"),
          "…and says who it is");
    check(res.value("agentCapabilities")
              .toObject()
              .value("promptCapabilities")
              .toObject()
              .value("image")
              .toBool(),
          "…and advertises image prompts, because Vision works on this path too");

    const QJsonObject made =
        rpc(R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":"/tmp"}})");
    check(made.value("result").toObject().value("sessionId").toString().startsWith(
              QStringLiteral("acp_")),
          "session/new hands back a session id");

    const QJsonObject bad =
        rpc(R"({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"nope","prompt":[]}})");
    check(bad.contains("error"), "a prompt for an unknown session is a JSON-RPC error, not a crash");

    const QJsonObject huh = rpc(R"({"jsonrpc":"2.0","id":4,"method":"no/such/method"})");
    check(huh.value("error").toObject().value("code").toInt() == -32601,
          "an unknown method answers -32601 rather than dying");

    p.closeWriteChannel();  // stdin closed = the editor went away
    check(p.waitForFinished(5000) && p.exitCode() == 0,
          "the server exits cleanly when the editor closes the pipe");

    const QString src = readSource(QStringLiteral("core/Acp.cpp"));
    check(src.contains("class TurnThread"),
          "a turn runs on a WORKER — a blocked reader could never take session/cancel");
    check(src.contains("Permission::setAsker"),
          "every mutating tool becomes a session/request_permission the EDITOR answers");
    check(src.contains("s.answers.insert") && src.contains("answered.wakeAll"),
          "the reader routes a permission answer back to the parked worker");
}

// The LSP server, spoken to the way an editor speaks to it: Content-Length framing
// over stdio. documentSymbol / references / rename / formatting need no model, so
// they are testable for real.
static void testLsp() {
    const QString cli = QCoreApplication::applicationDirPath() + QStringLiteral("/../cli/ollamadev");
    if (!QFileInfo(cli).isExecutable()) {
        check(true, "ollamadev binary not built here — skipping the LSP protocol test");
        return;
    }
    QTemporaryDir proj;
    const QString path = proj.path() + QStringLiteral("/a.py");
    const QString text = QStringLiteral("def greet(name):\n    return name\n\nprint(greet('x'))\n");
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(text.toUtf8());
    f.close();
    const QString uri = QStringLiteral("file://") + path;

    QProcess p;
    p.setWorkingDirectory(proj.path());
    p.start(cli, {QStringLiteral("lsp")});
    if (!p.waitForStarted(5000)) {
        check(false, "the lsp server starts");
        return;
    }

    const auto send = [&p](const QJsonObject& o) {
        const QByteArray body = QJsonDocument(o).toJson(QJsonDocument::Compact);
        p.write("Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        p.waitForBytesWritten(2000);
    };
    // Content-Length framing: read the header, then exactly that many bytes. Getting
    // this wrong is the classic LSP bug where the client goes permanently quiet.
    const auto recv = [&p]() -> QJsonObject {
        QByteArray hdr;
        while (!hdr.contains("\r\n\r\n")) {
            if (!p.waitForReadyRead(10000)) return {};
            hdr += p.readAll();
        }
        const int split = hdr.indexOf("\r\n\r\n") + 4;
        int len = 0;
        for (const QByteArray& line : hdr.left(split).split('\n'))
            if (line.toLower().startsWith("content-length:"))
                len = line.mid(15).trimmed().toInt();
        QByteArray body = hdr.mid(split);
        while (body.size() < len) {
            if (!p.waitForReadyRead(10000)) break;
            body += p.readAll();
        }
        return QJsonDocument::fromJson(body.left(len)).object();
    };

    send(QJsonObject{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", "initialize"},
                     {"params", QJsonObject{{"rootUri", QStringLiteral("file://") + proj.path()}}}});
    const QJsonObject caps =
        recv().value("result").toObject().value("capabilities").toObject();
    // The PHP server advertised definitionProvider and never handled
    // textDocument/definition — so go-to-definition silently did nothing and looked
    // like a broken EDITOR. Advertise only what is implemented.
    check(caps.value("definitionProvider").toBool() && caps.value("renameProvider").toBool() &&
              caps.value("documentSymbolProvider").toBool() &&
              caps.value("referencesProvider").toBool(),
          "the server advertises exactly the methods it implements");

    send(QJsonObject{{"jsonrpc", "2.0"},
                     {"method", "textDocument/didOpen"},
                     {"params", QJsonObject{{"textDocument",
                                             QJsonObject{{"uri", uri}, {"text", text}, {"version", 1}}}}}});
    recv();  // the diagnostics notification

    send(QJsonObject{{"jsonrpc", "2.0"},
                     {"id", 2},
                     {"method", "textDocument/documentSymbol"},
                     {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}}}}});
    const QJsonArray syms = recv().value("result").toArray();
    check(!syms.isEmpty() &&
              syms.first().toObject().value("name").toString() == QLatin1String("greet"),
          "documentSymbol finds the function");

    const QJsonObject at{{"line", 0}, {"character", 5}};  // on `greet`
    send(QJsonObject{{"jsonrpc", "2.0"},
                     {"id", 3},
                     {"method", "textDocument/references"},
                     {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}},
                                            {"position", at}}}});
    check(recv().value("result").toArray().size() == 2,
          "references finds the definition AND the call");

    send(QJsonObject{{"jsonrpc", "2.0"},
                     {"id", 4},
                     {"method", "textDocument/rename"},
                     {"params", QJsonObject{{"textDocument", QJsonObject{{"uri", uri}}},
                                            {"position", at},
                                            {"newName", QStringLiteral("welcome")}}}});
    const QJsonObject edit = recv().value("result").toObject();
    const QJsonArray edits = edit.value("changes").toObject().value(uri).toArray();
    check(edits.size() == 2 &&
              edits.first().toObject().value("newText").toString() == QLatin1String("welcome"),
          "rename edits every reference — and reuses references(), so the two can never disagree");

    send(QJsonObject{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "shutdown"}});
    recv();
    send(QJsonObject{{"jsonrpc", "2.0"}, {"method", "exit"}});
    check(p.waitForFinished(5000) && p.exitCode() == 0, "the server shuts down cleanly");

    const QString src = readSource(QStringLiteral("core/Lsp.cpp"));
    check(src.contains("if (p.exitCode() != 0) return out;"),
          "a failed formatter yields NO edits — taking its stdout would replace the file "
          "with an error message");
}

// Interactive rebase, driven headlessly. Rewriting history is the most dangerous
// thing this app does, so the guards are tested as hard as the happy path.
static void testRebase() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        check(false, "temp dir for the rebase test");
        return;
    }
    const QString root = tmp.path();
    Tools::setThreadRoot(root);

    auto git = [&](const QStringList& a) {
        QProcess p;
        p.setWorkingDirectory(root);
        p.start(QStringLiteral("git"), a);
        return p.waitForFinished(10000) && p.exitCode() == 0;
    };
    if (!git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
              QStringLiteral("main")})) {
        check(true, "git is not installed — skipping the rebase tests");
        Tools::setThreadRoot(QString());
        return;
    }
    git({QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@t.t")});
    git({QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("t")});

    const auto commit = [&](const QString& line, const QString& msg) {
        QFile f(root + QStringLiteral("/f.txt"));
        f.open(QIODevice::Append);
        f.write(line.toUtf8() + "\n");
        f.close();
        git({QStringLiteral("add"), QStringLiteral("-A")});
        git({QStringLiteral("commit"), QStringLiteral("-qm"), msg});
    };
    commit(QStringLiteral("a"), QStringLiteral("add the parser"));
    commit(QStringLiteral("b"), QStringLiteral("wip"));
    commit(QStringLiteral("c"), QStringLiteral("fix typo"));
    commit(QStringLiteral("d"), QStringLiteral("add tests"));

    RebasePlan plan = Rebase::planFor(4);
    check(plan.steps.size() == 4, "planFor reads back the commits");
    check(plan.steps.first().subject == QStringLiteral("add the parser"),
          "the plan is OLDEST FIRST — which is git's todo order, and getting it "
          "backwards would fold every commit into the wrong one");
    check(plan.base == QStringLiteral("--root"),
          "rewriting everything back to the root is spelled --root, not HEAD~n");

    // Fold the two junk commits into the parser, keep the rest.
    plan.steps[1].action = RebaseStep::Fixup;
    plan.steps[2].action = RebaseStep::Fixup;
    plan.steps[3].action = RebaseStep::Reword;
    plan.steps[3].newMessage = QStringLiteral("test: add the tests");

    QString err;
    const RebaseResult r = Rebase::apply(plan, &err);
    check(r.ok, "the rebase runs headlessly — no editor, ever");
    check(!r.backupRef.isEmpty(), "a backup ref is taken BEFORE anything moves");

    QProcess log;
    log.setWorkingDirectory(root);
    log.start(QStringLiteral("git"),
              {QStringLiteral("log"), QStringLiteral("--oneline"), QStringLiteral("--format=%s")});
    log.waitForFinished(5000);
    const QStringList subjects =
        QString::fromUtf8(log.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    check(subjects.size() == 2, "4 commits became 2");
    check(subjects.first() == QStringLiteral("test: add the tests"),
          "reword replaced the message — without ever opening an editor");
    check(subjects.last() == QStringLiteral("add the parser"),
          "fixup folded the junk into the commit above and kept ITS message");

    // The content must survive. A rebase that loses a line is worse than no rebase.
    QFile f(root + QStringLiteral("/f.txt"));
    f.open(QIODevice::ReadOnly);
    const QString text = QString::fromUtf8(f.readAll());
    f.close();
    check(text.contains('a') && text.contains('b') && text.contains('c') && text.contains('d'),
          "EVERY line of code survives the rewrite");

    // Undo.
    check(Rebase::undo(r.backupRef, &err), "the backup ref undoes the whole rebase");
    log.start(QStringLiteral("git"), {QStringLiteral("log"), QStringLiteral("--format=%s")});
    log.waitForFinished(5000);
    check(QString::fromUtf8(log.readAllStandardOutput())
                  .split('\n', Qt::SkipEmptyParts)
                  .size() == 4,
          "…and all four original commits are back");

    // ---- the guards ----
    RebasePlan bad = Rebase::planFor(3);
    for (RebaseStep& s : bad.steps) s.action = RebaseStep::Drop;
    check(!Rebase::apply(bad, &err).ok, "a plan that drops every commit is refused");

    RebasePlan dirty = Rebase::planFor(3);
    QFile d(root + QStringLiteral("/f.txt"));
    d.open(QIODevice::Append);
    d.write("uncommitted\n");
    d.close();
    check(!Rebase::apply(dirty, &err).ok && err.contains(QStringLiteral("stash")),
          "a rebase over uncommitted TRACKED changes is refused");

    // …but an untracked file must NOT block it: git does not mind them, and this app
    // drops its own .ollamadev/ folder into every project it touches — so counting
    // those made the app's own droppings refuse the app's own rebase.
    git({QStringLiteral("checkout"), QStringLiteral("--"), QStringLiteral("f.txt")});
    QDir(root).mkpath(QStringLiteral(".ollamadev"));
    QFile junk(root + QStringLiteral("/.ollamadev/sessions.json"));
    junk.open(QIODevice::WriteOnly);
    junk.write("{}");
    junk.close();
    RebasePlan withJunk = Rebase::planFor(2);
    check(Rebase::apply(withJunk, &err).ok,
          "an UNTRACKED file does not block a rebase (the app's own .ollamadev did)");

    Tools::setThreadRoot(QString());
}

// A crew sandbox is a git WORKTREE when the project is under git.
//
// The whole reason this is not a two-line change: `git worktree add` checks out a
// COMMIT, but a coder must start from the user's WORKING TREE. Anything
// uncommitted would otherwise be invisible to it — and, far worse, capture() would
// compare the sandbox against the project, see the user's own uncommitted edits,
// and record them as changes the CODER had reverted. Landing that changeset would
// then silently undo the user's work. Every check below exists for that.
static void testWorktreeSandbox() {
    QTemporaryDir proj, sb;
    if (!proj.isValid()) {
        check(false, "temp dirs for the worktree test");
        return;
    }
    const QString root = proj.path();

    auto git = [&](const QStringList& a) {
        QProcess p;
        p.setWorkingDirectory(root);
        p.start(QStringLiteral("git"), a);
        return p.waitForFinished(15000) && p.exitCode() == 0;
    };
    if (!git({QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"),
              QStringLiteral("main")})) {
        check(true, "git is not installed — skipping the worktree tests");
        return;
    }
    git({QStringLiteral("config"), QStringLiteral("user.email"), QStringLiteral("t@t.t")});
    git({QStringLiteral("config"), QStringLiteral("user.name"), QStringLiteral("t")});

    const auto put = [](const QString& p, const char* text) {
        QDir().mkpath(QFileInfo(p).absolutePath());
        QFile f(p);
        f.open(QIODevice::WriteOnly);
        f.write(text);
        f.close();
    };
    const auto slurp = [](const QString& p) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    };

    put(root + "/committed.txt", "committed\n");
    put(root + "/deleteme.txt", "doomed\n");
    put(root + "/.gitignore", "ignored/\n");
    git({QStringLiteral("add"), QStringLiteral("-A")});
    git({QStringLiteral("commit"), QStringLiteral("-qm"), QStringLiteral("seed")});

    // Now make the project DIRTY, exactly as a real user's would be.
    put(root + "/committed.txt", "committed\nEDITED BUT NOT COMMITTED\n");  // modified
    put(root + "/untracked.txt", "brand new, never committed\n");            // untracked
    QFile::remove(root + "/deleteme.txt");                                    // deleted
    put(root + "/ignored/junk.o", "build output\n");                          // gitignored

    const QString box = sb.path() + QStringLiteral("/c1");
    QString err;
    check(Sandbox::create(root, box, &err), "create() makes a sandbox from a git project");
    check(QFileInfo(box + QStringLiteral("/.git")).isFile(),
          "…and it is a WORKTREE (a .git FILE), not a folder copy");

    // THE BUG THIS GUARDS. If the sandbox were left at HEAD, the coder would not see
    // the user's uncommitted work, and capture() would read it back as a deletion.
    check(slurp(box + QStringLiteral("/committed.txt")).contains(QStringLiteral("EDITED")),
          "the user's UNCOMMITTED edit is in the sandbox (worktree alone would miss it)");
    check(QFileInfo::exists(box + QStringLiteral("/untracked.txt")),
          "…and their untracked file is too");
    check(!QFileInfo::exists(box + QStringLiteral("/deleteme.txt")),
          "…and a file they deleted is really gone, not resurrected from HEAD");
    check(!QFileInfo::exists(box + QStringLiteral("/ignored/junk.o")),
          "gitignored build junk is absent — the folder copy used to drag it in");

    // The point of the whole migration: a coder can now actually use git.
    QProcess p;
    p.setWorkingDirectory(box);
    p.start(QStringLiteral("git"), {QStringLiteral("log"), QStringLiteral("--oneline")});
    p.waitForFinished(10000);
    check(p.exitCode() == 0 && QString::fromUtf8(p.readAllStandardOutput()).contains("seed"),
          "the coder can run git IN its sandbox — the 17 git_* tools were dead before this");

    // capture() must see nothing: an untouched sandbox is not a changeset. If the
    // dirty-state replay were wrong, this is where it would show up as phantom edits.
    QTemporaryDir store;
    const Changeset none = Sandbox::capture(root, box, store.path());
    check(none.empty(), "an untouched worktree sandbox captures NOTHING (no phantom changes)");

    // And a real coder edit is still captured.
    put(box + QStringLiteral("/committed.txt"), "committed\nEDITED BUT NOT COMMITTED\nCODER\n");
    QTemporaryDir store2;
    const Changeset cs = Sandbox::capture(root, box, store2.path());
    check(cs.modified.contains(QStringLiteral("committed.txt")),
          "…but a change the coder actually made IS captured");

    check(Sandbox::destroy(root, box), "destroy() removes the worktree");
    check(!QFileInfo::exists(box), "…and the directory is gone");
    QProcess wl;
    wl.setWorkingDirectory(root);
    wl.start(QStringLiteral("git"), {QStringLiteral("worktree"), QStringLiteral("list")});
    wl.waitForFinished(10000);
    check(!QString::fromUtf8(wl.readAllStandardOutput()).contains(QStringLiteral("/c1")),
          "…and it is unregistered from .git, so the path can be reused");

    // A project with no git at all must still work — that is what copyTree is for.
    QTemporaryDir plain, box2;
    put(plain.path() + QStringLiteral("/a.txt"), "hello\n");
    check(Sandbox::create(plain.path(), box2.path() + QStringLiteral("/c1"), &err),
          "a NON-git project still gets a sandbox (folder copy)");
    check(QFileInfo::exists(box2.path() + QStringLiteral("/c1/a.txt")),
          "…with its files in it");
    check(!QFileInfo(box2.path() + QStringLiteral("/c1/.git")).exists(),
          "…and no worktree, because there is no repo to make one from");
}

// git.model: an explicit setting always wins; with none set, a commit message is a
// small local job, so the default prefers gpt-oss:20b over the (often big, cloud)
// session model — but only falls to it, never breaks a fresh install that never
// pulled it.
static void testGitModel() {
    // Config::load() is load-once, so we can't re-read a file mid-run; drive the
    // LIVE config with setPref (which also updates it in memory). Point HOME at a
    // throwaway dir first so setPref writes to a scratch ade-prefs.json and never
    // touches the real one.
    const QByteArray realHome = qgetenv("HOME");
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());

    // An explicit setting is returned verbatim and never second-guessed — this is
    // the user's own choice (e.g. a big cloud model), and the default must not
    // override it.
    Config::setPref(QStringLiteral("git.model"), QStringLiteral("gpt-oss:120b-cloud"));
    check(GitFlow::modelFor(QStringLiteral("qwen3.5:9b")) == QStringLiteral("gpt-oss:120b-cloud"),
          "an explicit git.model ALWAYS wins over the default");

    // With none set, modelFor falls back to something usable — never a dangling
    // default, never empty.
    Config::setPref(QStringLiteral("git.model"), QString());
    check(GitFlow::modelFor(QStringLiteral("qwen3.5:9b")) != QString(),
          "with no git.model set, modelFor returns *something* usable");

    qputenv("HOME", realHome);
}

// ---- session auto-resume (Session::latestForCwd) ---------------------------
// The `-c` / folder auto-resume path. testExportImport already covers save/load
// fidelity; this pins the resume-SELECTION rules that were reworked this week.
static void testSessionResume() {
    QTemporaryDir proj;
    if (!proj.isValid()) {
        check(false, "temp project for the session-resume test");
        return;
    }
    const QString cwd = QDir::currentPath();
    QDir::setCurrent(proj.path());  // sessions live under the PROJECT's .ollamadev

    // An empty, just-created session is NOT something you resume into.
    Session empty = Session::create(proj.path());
    empty.save();
    check(!Session::latestForCwd(proj.path()).has_value(),
          "session: an empty session is not offered for resume");

    // One that actually has messages IS.
    Session s1 = Session::create(proj.path());
    ChatMessage m1;
    m1.role = QStringLiteral("user");
    m1.content = QStringLiteral("first");
    s1.messages() << m1;
    s1.save();
    auto got = Session::latestForCwd(proj.path());
    check(got.has_value() && got->id() == s1.id(),
          "session: latestForCwd returns the session that has messages");

    // The NEWEST session with messages wins (ids/mtimes are second-resolution).
    QThread::msleep(1100);
    Session s2 = Session::create(proj.path());
    ChatMessage m2;
    m2.role = QStringLiteral("user");
    m2.content = QStringLiteral("second");
    s2.messages() << m2;
    s2.save();
    auto latest = Session::latestForCwd(proj.path());
    check(latest.has_value() && latest->id() == s2.id(),
          "session: the newest session with messages wins");

    // Resume is per-project: a different folder sees none of these.
    check(!Session::latestForCwd(QStringLiteral("/nonexistent/other/repo")).has_value(),
          "session: latestForCwd is per-project (no cross-repo bleed)");

    QDir::setCurrent(cwd);
}

// --------------------------------------------------------------------------
// Argument handling. A CLI that ignores what it does not understand reports
// success for work it never did — these guard the three ways that used to
// happen: a mistyped flag, a mistyped subcommand, and colour written to a pipe.
// --------------------------------------------------------------------------

struct CliRun {
    int code = -1;
    QString out, err;
};

// `home` and `cwd`, when given, isolate the run: HOME decides where
// ~/.ollamadev (and so workspaces.json) is read from, so a test can set up a
// whole config without touching the developer's real one.
static CliRun runCli(const QStringList& args, const QByteArray& stdinData = {},
                     const QString& home = {}, const QString& cwd = {}) {
    CliRun r;
    QProcess p;
    p.setProgram(QStringLiteral(ODV_CLI_BIN));
    p.setArguments(args);
    if (!cwd.isEmpty()) p.setWorkingDirectory(cwd);
    if (!home.isEmpty()) {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home);
        p.setProcessEnvironment(env);
    }
    p.start();
    if (!p.waitForStarted(5000)) return r;
    p.write(stdinData);
    p.closeWriteChannel();
    if (!p.waitForFinished(30000)) {
        p.kill();
        p.waitForFinished(2000);
        return r;
    }
    r.code = p.exitCode();
    r.out = QString::fromUtf8(p.readAllStandardOutput());
    r.err = QString::fromUtf8(p.readAllStandardError());
    return r;
}

static void testCliArgChecking() {
    // A flag we do not know used to be dropped in silence: `models --jsn` printed
    // a plain list and exited 0, so a script piping it into jq failed on valid-
    // looking success.
    const CliRun badFlag = runCli({"models", "--jsn"});
    check(badFlag.code == 2, "a mistyped flag exits 2 instead of pretending to work");
    check(badFlag.err.contains(QStringLiteral("unknown option: --jsn")),
          "…and says which flag it was");
    check(badFlag.err.contains(QStringLiteral("--json")),
          "…and suggests the flag that was meant");
    check(badFlag.out.isEmpty(), "…and prints nothing to stdout, so a pipe sees no output");

    // The suggestion must not fire on a flag that is genuinely ours.
    const CliRun help = runCli({"--help"});
    check(help.code == 0 && help.out.contains(QStringLiteral("Usage:")),
          "a real flag is still accepted");

    // A lone word that is nearly a subcommand is a typo. It used to fall through
    // to the model, which answered it as a question — a wasted call.
    const CliRun typo = runCli({"comit"});
    check(typo.code == 2, "a mistyped subcommand exits 2");
    check(typo.err.contains(QStringLiteral("did you mean `ollamadev commit`")),
          "…and names the command that was meant");

    // …but a command that forwards its tail must NOT have that tail judged: git's
    // flags are git's business.
    const CliRun passthru = runCli({"git", "--this-is-gits-problem"});
    check(!passthru.err.contains(QStringLiteral("unknown option:")),
          "a pass-through command's flags are left to the program that owns them");

    // The escape hatch has to exist, because quoting cannot provide one: the shell
    // strips quotes, so `ollamadev "comit"` reaches main() as the same argv.
    check(help.out.contains(QStringLiteral("ollamadev -- <prompt>")),
          "--help documents `--` as the way to force prompt text");
}

// `help` is the first thing anyone types at a CLI they do not know. It used to be
// the one word that was neither a command nor a flag, so it fell through to the
// model and came back "Sure! What do you need help with?" — a wasted call, and a
// bewildering answer. None of these may touch the network.
static void testHelpIsACommand() {
    const CliRun index = runCli({"help"});
    check(index.code == 0, "`help` exits 0");
    check(index.out.contains(QStringLiteral("Usage: ollamadev")),
          "`help` prints usage rather than asking a model what you meant");
    check(!index.out.contains(QStringLiteral("What do you need help with")),
          "…and is answered locally, never by the model");

    // The index is the flat list you scan for a name, so every command must be on
    // it — including the ones the grouped --help text never mentions.
    for (const char* name : {"crew", "eval", "tidy", "acp", "ws", "plugin"})
        check(index.out.contains(QLatin1String(name)),
              QByteArray("`help` lists ") + name);

    // Asked about one command, answer about that command.
    const CliRun crew = runCli({"help", "crew"});
    check(crew.code == 0 && crew.out.contains(QStringLiteral("Usage: ollamadev crew")),
          "`help <cmd>` prints that command's usage");
    check(crew.out.contains(QStringLiteral("--amplify")),
          "…including its own flags, which the global help crowds out");
    check(!crew.out.contains(QStringLiteral("ollamadev doctor")),
          "…and not the whole command surface");

    // The two spellings are the same question and must give the same answer.
    const CliRun flag = runCli({"crew", "--help"});
    check(flag.code == 0 && flag.out == crew.out, "`<cmd> --help` == `help <cmd>`");

    const CliRun dashH = runCli({"commit", "-h"});
    check(dashH.code == 0 && dashH.out.contains(QStringLiteral("Usage: ollamadev commit")),
          "-h is per-command too");

    // A bare --help is still the grouped overview, not one command's page.
    const CliRun global = runCli({"--help"});
    check(global.out.contains(QStringLiteral("Crew — the parallel bench")),
          "a bare --help is still the full overview");

    // An unknown topic is a mistake, not a prompt.
    const CliRun bad = runCli({"help", "comit"});
    check(bad.code == 2, "`help <typo>` exits 2");
    check(bad.err.contains(QStringLiteral("did you mean `ollamadev help commit`")),
          "…and names the command that was meant");

    // A bare `git --help` asks about our wrapper, so we answer. With a subcommand
    // present the line is git's, and must reach git untouched.
    const CliRun bareGit = runCli({"git", "--help"});
    check(bareGit.out.contains(QStringLiteral("Usage: ollamadev git")),
          "a bare `git --help` describes our wrapper");
    const CliRun gitLog = runCli({"git", "log", "--help"});
    check(!gitLog.out.contains(QStringLiteral("Usage: ollamadev git")),
          "…but `git log --help` belongs to git and is forwarded");
}

// The completions were three hand-kept lists that had drifted twelve commands
// behind the dispatch: acp, agents, export, import, plugin, plugins, route, tidy,
// update, upgrade, workspace and ws could not be completed at all. They are
// generated from the command table now, and this is the test that keeps them
// honest — it fails the moment a new command is added without one.
static void testCompletionsCoverEveryCommand() {
    const CliRun index = runCli({"help"});
    QStringList commands;
    const QStringList lines = index.out.split('\n');
    for (const QString& line : lines) {
        if (!line.startsWith(QStringLiteral("  ")) || line.startsWith(QStringLiteral("   ")))
            continue;
        const QString name = line.trimmed().section(' ', 0, 0);
        if (!name.isEmpty() && !name.startsWith('-')) commands << name;
    }
    check(commands.size() > 40, "the help index lists the whole command surface");

    for (const QString& shell : {QStringLiteral("bash"), QStringLiteral("zsh"),
                                 QStringLiteral("fish")}) {
        const CliRun c = runCli({"completion", shell});
        check(c.code == 0 && !c.out.isEmpty(), "completion " + shell.toUtf8() + " prints a script");
        QStringList missing;
        for (const QString& cmd : commands)
            if (!c.out.contains(cmd)) missing << cmd;
        check(missing.isEmpty(),
              "completion " + shell.toUtf8() + " offers every command (missing: " +
                  missing.join(',').toUtf8() + ")");
    }

    // A blurb carrying an apostrophe ("someone else's") sits inside a single-quoted
    // zsh/fish literal, where an unescaped one ends the string and breaks the file.
    const CliRun zsh = runCli({"completion", "zsh"});
    check(zsh.out.contains(QStringLiteral("'\\''")), "zsh blurbs escape their apostrophes");
}

// The man page is generated from the command table rather than checked in, so
// these assertions are what stops it drifting from the binary. groff is not a
// build dependency, so the roff is checked structurally here instead.
static void testManPage() {
    const CliRun man = runCli({"man"});
    check(man.code == 0 && !man.out.isEmpty(), "`man` prints a page");
    check(man.out.startsWith(QStringLiteral(".TH OLLAMADEV 1")), "…starting with a .TH header");

    for (const char* section : {".SH NAME", ".SH SYNOPSIS", ".SH DESCRIPTION", ".SH COMMANDS",
                                ".SH OPTIONS", ".SH EXIT STATUS", ".SH ENVIRONMENT", ".SH FILES",
                                ".SH EXAMPLES", ".SH SEE ALSO"})
        check(man.out.contains(QLatin1String(section)),
              QByteArray("…and a ") + (section + 4) + " section");

    // Raw UTF-8 makes troff warn on every continuation byte, so the generator
    // spells the typography as roff glyphs. Nothing non-ASCII may survive.
    for (const QChar c : man.out)
        if (c.unicode() > 127) {
            check(false, "the man page is pure ASCII (roff glyphs, not raw UTF-8)");
            break;
        }
    check(man.out.contains(QStringLiteral("\\(em")), "…em dashes are roff glyphs");

    // Every command must be documented, which is the whole point of generating it.
    const CliRun index = runCli({"help"});
    QStringList missing;
    const QStringList lines = index.out.split('\n');
    for (const QString& line : lines) {
        if (!line.startsWith(QStringLiteral("  ")) || line.startsWith(QStringLiteral("   ")))
            continue;
        const QString name = line.trimmed().section(' ', 0, 0);
        if (name.isEmpty() || name.startsWith('-')) continue;
        // Names go through the same hyphen escaping the page does, so `code-search`
        // is looked for as `code\-search`.
        QString roff = name;
        roff.replace(QStringLiteral("-"), QStringLiteral("\\-"));
        if (!man.out.contains(QStringLiteral(".B ollamadev ") + roff)) missing << name;
    }
    check(missing.isEmpty(),
          "the man page documents every command (missing: " + missing.join(',').toUtf8() + ")");

    // The version in the header has to be the real one, or a packaged page claims
    // to describe a release it does not.
    const CliRun ver = runCli({"--version"});
    const QString v = ver.out.trimmed().section(' ', -1);
    check(!v.isEmpty() && man.out.contains(QStringLiteral("\"OllamaDev ") + v + '"'),
          "…and its header carries the running version");

    // --out is what the build uses; a silent failure there would ship an empty
    // page, so it must report what happened.
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("ollamadev.1"));
    const CliRun toFile = runCli({"man", "--out", path});
    check(toFile.code == 0, "`man --out <file>` succeeds");
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), "…and creates the file");
    check(QString::fromUtf8(f.readAll()) == man.out, "…with exactly what stdout would have got");

    const CliRun bad = runCli({"man", "--out", QStringLiteral("/nonexistent-dir/x.1")});
    check(bad.code != 0, "an unwritable --out is an error, not a silent empty page");
}

// Dispatch read args.first(), so a flag written before the command hid it:
// `ollamadev --json models` matched no command, fell through to the typo path,
// and suggested `ollamadev models` to someone who had just typed models. Every
// mainstream CLI takes global flags on either side of the subcommand.
static void testFlagsBeforeCommand() {
    const CliRun before = runCli({"--json", "models"});
    const CliRun after = runCli({"models", "--json"});
    check(before.code == 0, "a flag before the command still finds the command");
    check(!before.err.contains(QStringLiteral("unknown command")),
          "…and does not report the command it was given as unknown");
    check(before.out == after.out, "…and means exactly the same as the flag after it");

    // The flag has to REACH the subcommand, not merely be tolerated by dispatch.
    check(before.out.trimmed().startsWith('{'), "…with the flag actually applied");

    for (const char* f : {"--no-color", "--verbose", "-q"}) {
        const CliRun r = runCli({QString::fromLatin1(f), "backends"});
        check(r.code == 0, QByteArray(f) + " before the command works too");
    }

    // A command WITH a subcommand is the hard case, and the one the first version
    // of this fix broke. Handlers read their subcommand positionally, so a leading
    // flag left in place was read as the subcommand: `--no-color ws list` reported
    // an unknown subcommand "--no-color". The argument list is normalised now, and
    // both spellings have to agree exactly.
    const QVector<QStringList> subcommandForms{
        {QStringLiteral("ws"), QStringLiteral("list")},
        {QStringLiteral("memory"), QStringLiteral("list")},
        {QStringLiteral("index"), QStringLiteral("status")},
        {QStringLiteral("skills"), QStringLiteral("list")},
        {QStringLiteral("models"), QStringLiteral("presets")},
        {QStringLiteral("config"), QStringLiteral("get"), QStringLiteral("ui.color")},
    };
    for (const QStringList& form : subcommandForms) {
        for (const char* f : {"--no-color", "-q", "--verbose"}) {
            const QString flag = QString::fromLatin1(f);
            const CliRun pre = runCli(QStringList{flag} + form);
            const CliRun post = runCli(form + QStringList{flag});
            const QByteArray label = (form.join(' ') + " with " + flag).toUtf8();
            check(pre.code == post.code, label + ": same exit code either side");
            check(pre.out == post.out, label + ": same output either side");
        }
    }

    // …but a pass-through command's argv is not ours to reorder.
    const CliRun git = runCli({"git", "log", "--oneline", "-3"});
    check(git.code == 0 && git.out.count('\n') <= 3,
          "git's own argv survives in the order it was given");
}

// -q/--quiet and --verbose. The contract that matters: quiet drops PROGRESS only.
// A flag that could hide a failure would be a trap in a CI log.
static void testVerbosity() {
    const CliRun normal = runCli({"backends"});
    const CliRun quiet = runCli({"-q", "backends"});
    check(quiet.code == 0 && !quiet.out.isEmpty(),
          "--quiet still prints the output the command exists to produce");
    check(quiet.out == normal.out, "…unchanged: it suppresses progress, not results");

    // Errors must survive quiet, or a silent failure looks like success.
    const CliRun quietErr = runCli({"-q", "--nonsense"});
    check(quietErr.code == 2, "--quiet still fails on a bad flag");
    check(!quietErr.err.isEmpty(), "…and still says why, on stderr");

    // --verbose reports the resolved environment on STDERR, so turning it on
    // cannot corrupt output something downstream is parsing.
    const CliRun verbose = runCli({"--verbose", "backends"});
    check(verbose.out == normal.out, "--verbose leaves stdout byte-for-byte alone");
    for (const char* field : {"cwd", "backend", "model", "config"})
        check(verbose.err.contains(QLatin1String(field)),
              QByteArray("--verbose reports the resolved ") + field);

    // Both at once has to be deterministic rather than argv-order dependent.
    const CliRun both = runCli({"--quiet", "--verbose", "backends"});
    check(both.err.isEmpty() || !both.err.contains(QStringLiteral("cwd")),
          "--quiet wins when both are given");
}

// --json is a global flag, so it used to be accepted everywhere and honoured only
// in a few places: `doctor --json` printed the human table and dropped the flag,
// which a script would then parse as though it had worked.
static void testJsonCoverage() {
    // Run every command the allowlist claims, for real, and parse what comes back.
    // The hand-written first version of that list was wrong in both directions —
    // it claimed crew and ws emitted JSON when they had no --json path at all, and
    // omitted board, which did. Asserting it by execution is the only version of
    // this check that cannot quietly become false.
    //
    // eval and diff need a model or a git repo, so they are exercised elsewhere;
    // everything else runs standalone. Each is given its list form, which is what
    // a bare `<cmd> --json` reaches.
    const QVector<QPair<QString, QStringList>> listForms{
        {QStringLiteral("backends"), {QStringLiteral("backends")}},
        {QStringLiteral("board"), {QStringLiteral("board")}},
        {QStringLiteral("memory"), {QStringLiteral("memory"), QStringLiteral("list")}},
        {QStringLiteral("models"), {QStringLiteral("models")}},
        {QStringLiteral("scan"), {QStringLiteral("scan")}},
        {QStringLiteral("ws"), {QStringLiteral("ws"), QStringLiteral("list")}},
    };
    for (const auto& form : listForms) {
        QStringList argv = form.second;
        argv << QStringLiteral("--json");
        const CliRun r = runCli(argv);
        check(r.code == 0 || r.code == 1,
              form.first.toUtf8() + " --json runs (0, or 1 for a finding)");
        QJsonParseError e{};
        QJsonDocument::fromJson(r.out.toUtf8(), &e);
        check(e.error == QJsonParseError::NoError,
              form.first.toUtf8() + " --json emits parseable JSON, not prose");
    }

    // Empty state must still be JSON. These are run against a fresh HOME, where
    // there are no workspaces and no notes — the case that used to print a
    // friendly sentence straight into somebody's parser.
    QTemporaryDir emptyHome;
    if (emptyHome.isValid()) {
        for (const char* cmd : {"ws", "memory"}) {
            const CliRun r =
                runCli({QString::fromLatin1(cmd), "list", "--json"}, {}, emptyHome.path());
            check(r.out.trimmed() == QStringLiteral("[]"),
                  QByteArray(cmd) + " list --json is [] when empty, not a sentence");
        }
    }

    // `board` honoured --json long before the allowlist existed. Leaving it out
    // turned a working flag into an error, which is the more expensive direction
    // to be wrong in — so it gets its own assertion rather than living only in the
    // loop above.
    const CliRun board = runCli({"board", "--json"});
    check(board.code == 0 && !board.err.contains(QStringLiteral("not supported")),
          "board --json is not rejected — it has always emitted JSON");

    // A command with no JSON form must say so rather than pretend.
    const CliRun unsupported = runCli({"doctor", "--json"});
    check(unsupported.code == 2, "--json on a command that has none exits 2");
    check(unsupported.err.contains(QStringLiteral("not supported")),
          "…and says the flag is unsupported there");
    check(unsupported.err.contains(QStringLiteral("backends")),
          "…and lists the commands that do emit JSON");
    check(unsupported.out.isEmpty(),
          "…and prints nothing to stdout, so a parser sees no half-answer");
}

// Crew runs were never collected: an interrupted run left its directory behind
// and nothing removed it, so they accumulated for months. Two consequences, both
// fixed here.
//
// Pruning deletes, which is the one thing in this CLI that cannot be undone, so
// what it REFUSES to touch matters more than what it removes. These assertions
// are that refusal.
static void testCrewPrune() {
    QTemporaryDir home, work;
    if (!home.isValid() || !work.isValid()) {
        check(false, "temp dirs for the prune test");
        return;
    }
    const QString state = home.filePath(QStringLiteral("state"));
    const QString crew = state + QStringLiteral("/crew");
    QDir().mkpath(crew);
    QDir().mkpath(state + QStringLiteral("/board"));

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const auto seed = [&](int daysAgo, const QString& task) {
        const QString id = QStringLiteral("crew_%1").arg(now - qint64(daysAgo) * 86400);
        QDir().mkpath(crew + QLatin1Char('/') + id);
        QFile f(crew + QLatin1Char('/') + id + QStringLiteral("/plan.json"));
        if (f.open(QIODevice::WriteOnly))
            f.write(QStringLiteral("{\"task\":\"%1\",\"cwd\":\"%2\",\"subtasks\":[{\"n\":1}]}")
                        .arg(task, work.path())
                        .toUtf8());
        return id;
    };
    const QString ancient = seed(90, QStringLiteral("ancient"));
    const QString old = seed(60, QStringLiteral("old"));
    const QString held = seed(45, QStringLiteral("held work"));
    const QString recent = seed(2, QStringLiteral("recent"));
    const QString live = seed(120, QStringLiteral("live"));  // OLDEST, and protected

    const auto write = [](const QString& path, const QString& body) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(body.toUtf8());
    };
    write(crew + QStringLiteral("/current.json"),
          QStringLiteral("{\"runId\":\"%1\",\"subtasks\":[]}").arg(live));
    write(state + QStringLiteral("/board/current.json"),
          QStringLiteral("{\"pending\":[{\"id\":\"d1\",\"kind\":\"crew_branch\",\"summary\":\"h\","
                         "\"data\":{\"runId\":\"%1\",\"n\":1}}]}")
              .arg(held));

    const auto run = [&](const QStringList& a) {
        QProcess p;
        p.setProgram(QStringLiteral(ODV_CLI_BIN));
        p.setArguments(a);
        p.setWorkingDirectory(work.path());
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert(QStringLiteral("HOME"), home.path());
        e.insert(QStringLiteral("OLLAMADEV_HOME"), state);
        p.setProcessEnvironment(e);
        p.start();
        CliRun r;
        if (!p.waitForStarted(5000) || !p.waitForFinished(30000)) {
            p.kill();
            return r;
        }
        r.code = p.exitCode();
        r.out = QString::fromUtf8(p.readAllStandardOutput());
        r.err = QString::fromUtf8(p.readAllStandardError());
        return r;
    };
    const auto exists = [&](const QString& id) {
        return QFileInfo::exists(crew + QLatin1Char('/') + id);
    };

    const CliRun dry = run({"crew", "prune", "--dry-run"});
    check(dry.code == 0, "`crew prune --dry-run` succeeds");
    check(dry.out.contains(ancient) && dry.out.contains(old), "…and names what it would remove");
    check(!dry.out.contains(live), "…never the live run, though it is the oldest of all");
    check(!dry.out.contains(held), "…never a run with a changeset still on the board");
    check(!dry.out.contains(recent), "…never one inside the age window");
    check(exists(ancient) && exists(old), "…and --dry-run removes nothing");

    // Unattended and unconfirmed must not delete: silence is not consent for
    // something irreversible.
    const CliRun unattended = run({"crew", "prune"});
    check(unattended.code == 1, "prune with no tty and no --yes refuses");
    check(exists(ancient), "…and leaves everything in place");

    const CliRun done = run({"crew", "prune", "--yes"});
    check(done.code == 0 && done.out.contains(QStringLiteral("removed 2")),
          "`crew prune --yes` removes exactly the eligible runs");
    check(!exists(ancient) && !exists(old), "…they are gone");
    check(exists(live), "…the live run survives");
    check(exists(held), "…the run with held work survives");
    check(exists(recent), "…and so does the recent one");

    // A bare `crew resume` must not adopt another folder's plan. Every seeded run
    // belongs to `work`, so from an unrelated directory there is nothing to take.
    QTemporaryDir elsewhere;
    if (elsewhere.isValid()) {
        QProcess p;
        p.setProgram(QStringLiteral(ODV_CLI_BIN));
        p.setArguments({QStringLiteral("crew"), QStringLiteral("resume")});
        p.setWorkingDirectory(elsewhere.path());
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.insert(QStringLiteral("HOME"), home.path());
        e.insert(QStringLiteral("OLLAMADEV_HOME"), state);
        p.setProcessEnvironment(e);
        p.start();
        p.waitForFinished(30000);
        const QString msg = QString::fromUtf8(p.readAllStandardError());
        check(p.exitCode() == 1, "a bare `crew resume` in an unrelated folder refuses");
        check(msg.contains(QStringLiteral("no run from this folder")),
              "…saying the runs on disk belong elsewhere");
        check(msg.contains(QStringLiteral("crew resume list")), "…and how to see them");
    }
}

// Invariants that must hold for EVERY command, enumerated from `ollamadev help`
// rather than from a list kept here — so a command added tomorrow is swept
// tomorrow, without anyone remembering to add it.
//
// These started as a throwaway shell sweep. A sweep that runs once proves the
// state of one afternoon; the same checks in the suite prove it on every build,
// which is the only version worth having.
static QStringList allCommands() {
    QStringList out;
    const QStringList lines = runCli({"help"}).out.split('\n');
    for (const QString& line : lines) {
        // The index is exactly two spaces, the name, then padding.
        if (!line.startsWith(QStringLiteral("  ")) || line.startsWith(QStringLiteral("   ")))
            continue;
        const QString name = line.trimmed().section(' ', 0, 0);
        if (!name.isEmpty() && !name.startsWith('-')) out << name;
    }
    return out;
}

static void testEveryCommandInvariants() {
    const QStringList commands = allCommands();
    check(commands.size() >= 47, "the help index enumerates the whole command surface");

    QStringList noHelp, wrongUsage, formsDiffer, dumpedGlobal;
    for (const QString& c : commands) {
        const CliRun viaWord = runCli({"help", c});
        const CliRun viaFlag = runCli({c, "--help"});

        if (viaWord.code != 0 || viaFlag.code != 0) noHelp << c;
        if (!viaWord.out.startsWith(QStringLiteral("Usage: ollamadev ") + c)) wrongUsage << c;
        if (viaWord.out != viaFlag.out) formsDiffer << c;
        // The whole point of per-command help is NOT reciting the grouped overview.
        if (viaWord.out.contains(QStringLiteral("Crew — the parallel bench"))) dumpedGlobal << c;
    }
    check(noHelp.isEmpty(), "every command has help that exits 0 (bad: " + noHelp.join(',').toUtf8() + ")");
    check(wrongUsage.isEmpty(),
          "every command's help opens with its own usage line (bad: " + wrongUsage.join(',').toUtf8() + ")");
    check(formsDiffer.isEmpty(),
          "`help X` and `X --help` agree for every command (bad: " + formsDiffer.join(',').toUtf8() + ")");
    check(dumpedGlobal.isEmpty(),
          "no command's help dumps the global overview (bad: " + dumpedGlobal.join(',').toUtf8() + ")");

    // Global flags must work on either side of the command, for every command
    // that can be run without a model or a network round-trip.
    const QStringList safe{"board",  "stats",   "agents", "hooks", "commands",
                           "help",   "man",     "skills", "memory", "ws"};
    QStringList positionMismatch;
    for (const QString& c : safe) {
        for (const char* f : {"--no-color", "-q", "--verbose"}) {
            const QString flag = QString::fromLatin1(f);
            const CliRun pre = runCli({flag, c});
            const CliRun post = runCli({c, flag});
            if (pre.code != post.code || pre.out != post.out)
                positionMismatch << (c + QLatin1Char('/') + flag);
        }
    }
    check(positionMismatch.isEmpty(),
          "a global flag means the same before and after the command (bad: " +
              positionMismatch.join(',').toUtf8() + ")");

    // A rejected command line must print nothing to stdout — otherwise something
    // parsing the output sees a half-answer and an error code it may not check.
    QStringList leaked;
    const QVector<QStringList> badLines{{QStringLiteral("--nonsense")},
                                        {QStringLiteral("help"), QStringLiteral("comit")},
                                        {QStringLiteral("index"), QStringLiteral("bogus")},
                                        {QStringLiteral("agents"), QStringLiteral("bogus")},
                                        {QStringLiteral("doctor"), QStringLiteral("--json")},
                                        {QStringLiteral("config"), QStringLiteral("get")}};
    for (const QStringList& line : badLines) {
        const CliRun r = runCli(line);
        if (r.code == 2 && !r.out.isEmpty()) leaked << line.join(' ');
    }
    check(leaked.isEmpty(), "a rejected line writes nothing to stdout (bad: " + leaked.join(';').toUtf8() + ")");
}

// A positional value beginning with a dash had no way through at all. The flag
// checker rejected `config set k -1` as an unknown option, and `--` was always
// read as "prompt text follows", so `config set k -- -1` sent "-1" to the model.
// A negative number was simply not expressible. After a command, `--` now means
// POSIX end-of-options; with no command it still forces prompt text.
static void testEndOfOptions() {
    QTemporaryDir home;
    if (!home.isValid()) {
        check(false, "temp HOME for the end-of-options test");
        return;
    }
    const auto cli = [&](const QStringList& a) { return runCli(a, {}, home.path()); };

    for (const char* v : {"-1", "-0.5", "-Wall", "--fast"}) {
        const QString value = QString::fromLatin1(v);
        const CliRun set = cli({"config", "set", "t.v", "--", value});
        check(set.code == 0, QByteArray("config set … -- ") + v + " succeeds");
        check(cli({"config", "get", "t.v"}).out.trimmed() == value,
              QByteArray("…and the value round-trips as ") + v);
    }

    // Without the --, it is still an error — but one that names the escape hatch
    // that actually applies to a command, not the prompt one.
    const CliRun bare = cli({"config", "set", "t.v", "-1"});
    check(bare.code == 2, "a dash-leading value without -- is still rejected");
    check(bare.err.contains(QStringLiteral("-- -1")),
          "…and the error shows the -- form that would work");
    check(!bare.err.contains(QStringLiteral("did you mean")),
          "…and does not guess a flag for a number");

    // A real typo must still get its suggestion.
    check(cli({"models", "--jsn"}).err.contains(QStringLiteral("did you mean --json")),
          "a genuine flag typo is still corrected");

    // With no command, -- keeps meaning prompt text: `-- backends` asks the model
    // about backends rather than running the command.
    const CliRun prompt = cli({"--", "backends"});
    check(!prompt.out.contains(QStringLiteral("Native tools")),
          "with no command, -- still forces prompt text rather than running it");
}

// Several commands dispatched their known subcommands and let everything else
// fall through to a default: `index buidl` reported the index status, `agents foo`
// listed every agent. Both look like they worked, which is the silent no-op this
// CLI rejects at the top level and had not been rejecting one level down.
static void testUnknownSubcommands() {
    const QVector<QPair<QStringList, QString>> cases{
        {{QStringLiteral("index"), QStringLiteral("buidl")}, QStringLiteral("index")},
        {{QStringLiteral("agents"), QStringLiteral("bogus")}, QStringLiteral("agents")},
    };
    for (const auto& c : cases) {
        const CliRun r = runCli(c.first);
        check(r.code == 2, c.second.toUtf8() + ": an unknown subcommand exits 2");
        check(r.err.contains(QStringLiteral("unknown subcommand")),
              c.second.toUtf8() + ": …and says so");
        check(r.err.contains(QStringLiteral("usage:")),
              c.second.toUtf8() + ": …and shows what it does take");
        check(r.out.isEmpty(),
              c.second.toUtf8() + ": …and prints nothing that looks like a result");
    }

    // The real subcommands must be untouched by that guard.
    check(runCli({"agents"}).code == 0, "a bare `agents` still lists them");
    check(runCli({"agents", "list"}).code == 0, "…and so does `agents list`");
    const CliRun idx = runCli({"index", "status"});
    check(idx.code == 0 || idx.code == 1, "`index status` still reports status");
}

// State used to live in ~/.ollamadev unconditionally. It now follows the XDG base
// directory spec for a fresh install — but an existing ~/.ollamadev keeps working
// untouched, because migrating it is how you lose somebody's sessions.
//
// The order under test is: $OLLAMADEV_HOME, then an existing ~/.ollamadev, then
// $XDG_DATA_HOME/ollamadev, then ~/.local/share/ollamadev.
static void testXdgPaths() {
    // `doctor` prints the resolved directory, which is the only way to observe
    // this from outside the process.
    const auto resolved = [](const QString& home, const QStringList& env) {
        QProcess p;
        p.setProgram(QStringLiteral(ODV_CLI_BIN));
        p.setArguments({QStringLiteral("doctor")});
        QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
        e.remove(QStringLiteral("XDG_DATA_HOME"));
        e.remove(QStringLiteral("OLLAMADEV_HOME"));
        e.insert(QStringLiteral("HOME"), home);
        for (int i = 0; i + 1 < env.size(); i += 2) e.insert(env.at(i), env.at(i + 1));
        p.setProcessEnvironment(e);
        p.start();
        if (!p.waitForStarted(5000) || !p.waitForFinished(30000)) {
            p.kill();
            return QString();
        }
        const QStringList lines = QString::fromUtf8(p.readAllStandardOutput()).split('\n');
        for (const QString& l : lines)
            if (l.startsWith(QStringLiteral("config"))) return l.section(' ', 1).trimmed();
        return QString();
    };

    QTemporaryDir fresh, legacyHome, xdg, explicitDir;
    if (!fresh.isValid() || !legacyHome.isValid() || !xdg.isValid() || !explicitDir.isValid()) {
        check(false, "temp dirs for the XDG test");
        return;
    }
    // A pre-existing ~/.ollamadev is what marks an install as legacy.
    QDir().mkpath(legacyHome.filePath(QStringLiteral(".ollamadev")));

    check(resolved(fresh.path(), {}) ==
              fresh.filePath(QStringLiteral(".local/share/ollamadev")),
          "a fresh install lands in ~/.local/share/ollamadev");

    check(resolved(fresh.path(), {QStringLiteral("XDG_DATA_HOME"), xdg.path()}) ==
              xdg.filePath(QStringLiteral("ollamadev")),
          "XDG_DATA_HOME is honoured");

    // The spec says a relative XDG_DATA_HOME is invalid and must be ignored.
    check(resolved(fresh.path(), {QStringLiteral("XDG_DATA_HOME"), QStringLiteral("rel/ative")}) ==
              fresh.filePath(QStringLiteral(".local/share/ollamadev")),
          "a relative XDG_DATA_HOME is ignored, per the spec");

    check(resolved(legacyHome.path(), {QStringLiteral("XDG_DATA_HOME"), xdg.path()}) ==
              legacyHome.filePath(QStringLiteral(".ollamadev")),
          "an existing ~/.ollamadev keeps winning — nothing is migrated");

    check(resolved(legacyHome.path(), {QStringLiteral("XDG_DATA_HOME"), xdg.path(),
                                       QStringLiteral("OLLAMADEV_HOME"), explicitDir.path()}) ==
              explicitDir.path(),
          "OLLAMADEV_HOME overrides everything");

    // A fresh install must not resurrect the legacy directory as a side effect —
    // that would silently flip every later run back onto the legacy branch.
    check(!QFileInfo::exists(fresh.filePath(QStringLiteral(".ollamadev"))),
          "…and a fresh install never creates ~/.ollamadev");
}

// Colour was controllable only by environment or config. A flag is what a script
// reaches for, and --color has to force colour back ON for pipes that do render
// escapes (less -R, CI logs) — so it overrides the not-a-tty test in both
// directions.
static void testColorFlags() {
    const QString esc = QStringLiteral("\033[");

    // stdout is a pipe here, so the baseline is already colourless; --color must
    // put it back.
    const CliRun forced = runCli({"--color", "help"});
    check(forced.out.contains(esc), "--color emits colour even when stdout is a pipe");

    const CliRun plain = runCli({"help"});
    check(!plain.out.contains(esc), "…and a pipe is otherwise still colourless");

    const CliRun off = runCli({"--no-color", "help"});
    check(!off.out.contains(esc), "--no-color stays colourless");

    // Both spellings must be known flags, or the arg checker rejects them.
    check(forced.code == 0 && off.code == 0, "--color and --no-color are accepted flags");
}

// The CLI must act on the folder it was run in. It used to chdir into the
// "active workspace" whenever cwd was not itself bookmarked — so with $HOME
// bookmarked and active, a one-shot in a scratch directory went and edited files
// in the home directory, under a permission mode that approves its own writes.
static void testCwdWinsOverActiveWorkspace() {
    QTemporaryDir home, elsewhere, proj;
    if (!home.isValid() || !elsewhere.isValid() || !proj.isValid()) {
        check(false, "cwd/workspace: temp dirs");
        return;
    }
    QDir().mkpath(home.path() + QStringLiteral("/.ollamadev"));

    // An active workspace pointing somewhere else entirely, and a project folder
    // that is deliberately NOT bookmarked.
    QJsonObject ws;
    ws.insert(QStringLiteral("id"), QStringLiteral("ws_elsewhere"));
    ws.insert(QStringLiteral("name"), QStringLiteral("elsewhere"));
    ws.insert(QStringLiteral("path"), elsewhere.path());
    QJsonObject wsFile;
    wsFile.insert(QStringLiteral("active"), QStringLiteral("ws_elsewhere"));
    wsFile.insert(QStringLiteral("workspaces"), QJsonArray{ws});
    QFile f(home.path() + QStringLiteral("/.ollamadev/workspaces.json"));
    check(f.open(QIODevice::WriteOnly), "cwd/workspace: wrote a workspaces.json");
    f.write(QJsonDocument(wsFile).toJson());
    f.close();

    // The REPL is the probe: it is one of the paths that reconciles the working
    // directory, and /pwd reports the answer without needing a model.
    const QByteArray script = QByteArrayLiteral("/pwd\n/exit\n");

    const CliRun stay = runCli({}, script, home.path(), proj.path());
    if (!stay.out.contains(QStringLiteral("/pwd")) && stay.out.trimmed().isEmpty()) {
        check(true, "cwd/workspace: REPL unavailable here — skipped");
        return;
    }
    check(stay.out.contains(proj.path()),
          "a run in an unbookmarked folder acts on THAT folder, not the active workspace");
    check(!stay.out.contains(elsewhere.path()) && !stay.err.contains(QStringLiteral("working in")),
          "…and does not redirect itself out of the launch directory");

    // The desktop-style behaviour is still reachable, but only on request.
    QFile cfg(home.path() + QStringLiteral("/.ollamadev/config.json"));
    check(cfg.open(QIODevice::WriteOnly), "cwd/workspace: wrote a config.json");
    cfg.write(QByteArrayLiteral("{\"workspace\":{\"follow\":true}}"));
    cfg.close();
    const CliRun follow = runCli({}, script, home.path(), proj.path());
    check(follow.out.contains(elsewhere.path()) || follow.err.contains(QStringLiteral("working in")),
          "workspace.follow=true opts back in to following the active project");
}

static void testColorGating() {
    // The suite's stdout is a pipe, which is exactly the case that used to spray
    // escape bytes into redirected output.
    check(!Tui::colorEnabled(), "colour is off when stdout is not a tty");

    // Belt and braces: the environment gates must hold independently of the tty.
    const QByteArray noColor = qgetenv("NO_COLOR");
    qputenv("NO_COLOR", "1");
    check(!Tui::colorEnabled(), "NO_COLOR turns colour off");
    check(Tui::paint(ansi::kGreen) == QLatin1String(""),
          "paint() yields an empty escape when colour is off");
    if (noColor.isEmpty())
        qunsetenv("NO_COLOR");
    else
        qputenv("NO_COLOR", noColor);

    // The banner, the prompt and /help all used to go out coloured regardless.
    const CliRun repl = runCli({}, QByteArray("/exit\n"));
    check(!repl.out.contains(QLatin1Char('\033')),
          "the REPL writes no escape bytes when its output is a pipe");
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    Config::load();

    QTextStream s(stdout);
    s << "json\n";       s.flush(); testJson();
    s << "models\n";     s.flush(); testModels();
    s << "secscan\n";    s.flush(); testSecScan();
    s << "sandbox\n";    s.flush(); testSandbox();
    s << "parallel\n";   s.flush(); testLimiter();
    s << "backends\n";   s.flush(); testBackends();
    s << "regressions\n"; s.flush(); testNoGlobalProcessNuking(); testCliArgvIsCurrent(); testCliBackendIsSandboxed(); testPerBackendModelRouting();
    s << "crew-resume\n"; s.flush(); testCrewResume();
    s << "crew-amplify\n"; s.flush(); testCrewAmplify();
    s << "agent-tools\n"; s.flush(); testAgentTools();
    s << "vision\n";      s.flush(); testVision();
    s << "crew-steer\n";  s.flush(); testCrewSteer(); testCrewSteerRefusesTheUnreachable();
    s << "workspaces\n"; s.flush(); testWorkspaces();
    s << "plugins\n";    s.flush(); testPlugins();
    s << "export\n";     s.flush(); testExportImport();
    s << "update\n";     s.flush(); testUpdateSafety();
    s << "acp\n";        s.flush(); testAcp();
    s << "lsp\n";        s.flush(); testLsp();
    s << "rebase\n";     s.flush(); testRebase();
    s << "worktree\n";   s.flush(); testWorktreeSandbox();
    s << "git-model\n";  s.flush(); testGitModel();
    s << "session-resume\n"; s.flush(); testSessionResume();
    s << "cli-args\n";   s.flush(); testCliArgChecking();
    s << "cwd\n";        s.flush(); testCwdWinsOverActiveWorkspace();
    s << "color\n";      s.flush(); testColorGating();
    s << "help\n";       s.flush(); testHelpIsACommand();
    s << "completions\n"; s.flush(); testCompletionsCoverEveryCommand();
    s << "color-flags\n"; s.flush(); testColorFlags();
    s << "man\n";        s.flush(); testManPage();
    s << "xdg\n";        s.flush(); testXdgPaths();
    s << "argorder\n";   s.flush(); testFlagsBeforeCommand();
    s << "verbosity\n";  s.flush(); testVerbosity();
    s << "json\n";       s.flush(); testJsonCoverage();
    s << "subcmds\n";    s.flush(); testUnknownSubcommands();
    s << "endopts\n";    s.flush(); testEndOfOptions();
    s << "invariants\n"; s.flush(); testEveryCommandInvariants();
    s << "crew-prune\n"; s.flush(); testCrewPrune();

    s << "\n" << passed << " passed, " << failed << " failed\n";
    s.flush();
    return failed == 0 ? 0 : 1;
}
