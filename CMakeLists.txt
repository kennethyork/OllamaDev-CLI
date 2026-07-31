cmake_minimum_required(VERSION 3.21)
project(ollamadev-cli VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# This is the headless CLI — no GUI toolchain, no Qt Widgets. Just Core, Network
# and Concurrent, so it builds on a server that has Qt Core but no desktop packages.
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Network Concurrent)

# CI (or a release build) can pass -DODV_RELEASE_VERSION=x.y.z so the binary reports
# the real published version. Empty → the value baked into core/Version.h is used.
if(DEFINED ODV_RELEASE_VERSION AND NOT ODV_RELEASE_VERSION STREQUAL "")
    add_compile_definitions(ODV_VERSION="${ODV_RELEASE_VERSION}")
endif()

if(COMMAND qt_standard_project_setup)
    qt_standard_project_setup()
endif()

add_subdirectory(core)
add_subdirectory(cli)

enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Install + packaging (CLI only).
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ollamadev RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(CPACK_PACKAGE_NAME "ollamadev")
set(CPACK_PACKAGE_VENDOR "OllamaDev")
if(DEFINED ODV_RELEASE_VERSION AND NOT ODV_RELEASE_VERSION STREQUAL "")
    set(CPACK_PACKAGE_VERSION "${ODV_RELEASE_VERSION}")
else()
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
endif()
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Local AI coding agent — Ollama and every major coding CLI, in your terminal")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/kennethyork/OllamaDev")
set(CPACK_PACKAGE_FILE_NAME "ollamadev-cli-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)

if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
    set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0")
    set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "OllamaDev <noreply@ollamadev.local>")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # Let dpkg-shlibdeps / rpmbuild derive the Qt/libc deps from the built ELF —
    # a hand-written list goes stale the first time Qt bumps a soname.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)
