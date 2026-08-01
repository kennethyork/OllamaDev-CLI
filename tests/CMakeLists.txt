# The CLI smoke suite. Everything it exercises lives in odv-core; the desktop-only
# tests (GitGraph lane assignment, the VtParser terminal emulator) stay in the ADE
# repo where that code lives.
add_executable(odv-tests smoke.cpp)
target_link_libraries(odv-tests PRIVATE odv-core Qt6::Core Qt6::Network)
# The regression tests read the sources: some bugs they guard are bugs of
# omission, which no happy-path run can catch.
target_compile_definitions(odv-tests PRIVATE ODV_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Argument handling lives in cli/main.cpp's anonymous namespace and cannot be
# linked against, so the tests that cover it run the real binary. They only use
# paths that exit before any network call, so this stays CI-safe.
target_compile_definitions(odv-tests PRIVATE ODV_CLI_BIN="$<TARGET_FILE:ollamadev>")
add_dependencies(odv-tests ollamadev)
add_test(NAME smoke COMMAND odv-tests)
