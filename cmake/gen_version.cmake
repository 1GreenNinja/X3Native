# Regenerates version_gen.h on EVERY build (driven by an always-run custom target).
# Stamps the git short hash + AUTO-INCREMENTING build number (commit count) +
# dirty flag + build timestamp so BOTH the window title AND the title screen tell
# you EXACTLY which build you are running. (Owner: "put an auto-incrementing
# VERSION NUMBER in the TITLE" — the root cause of nights of "which build am I
# playing?" debugging.)
#
# ONE SOURCE OF TRUTH: the window title (X3_VERSION_STR) and the title-screen
# stamp (X3_VERSION_STRING) are both derived from the values computed here, so
# they can never disagree.

# Short commit SHA (e.g. 47e37194).
execute_process(
    COMMAND git -C "${SRC}" rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_SHA)
    set(GIT_SHA "nogit")
endif()

# AUTO-INCREMENT build number = total commit count on HEAD. Bumps by itself with
# every commit — zero manual maintenance. This is the number Tim asked for.
execute_process(
    COMMAND git -C "${SRC}" rev-list --count HEAD
    OUTPUT_VARIABLE BUILD_NUMBER OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT BUILD_NUMBER)
    set(BUILD_NUMBER "0")
endif()

# Uncommitted changes? mark the build "+dirty" so a local edit is never mistaken
# for the committed hash.
execute_process(
    COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(DIRTY_SFX "")
if(GIT_DIRTY)
    set(DIRTY_SFX "+dirty")
endif()
# GIT_HASH keeps the legacy shape (sha + optional +dirty) for any existing readers.
set(GIT_HASH "${GIT_SHA}${DIRTY_SFX}")

# Bumpable manual major.minor in front of the auto-increment patch (commit count):
# v0.1.<count>. Bump MAJORMINOR by hand at a milestone; the patch rides commits.
set(MAJORMINOR "0.1")
set(VERSION_NUM "v${MAJORMINOR}.${BUILD_NUMBER}${DIRTY_SFX}")

string(TIMESTAMP BUILD_DATE "%Y-%m-%d" UTC)
string(TIMESTAMP BUILD_TS   "%Y-%m-%d %H:%M" UTC)

# Title-screen stamp: "v0.1.1247  47e37194  2026-07-26" (low-key build stamp).
set(VERSION_STRING "${VERSION_NUM}  ${GIT_SHA}  ${BUILD_DATE}")

set(CONTENT "#pragma once\n\
// AUTO-GENERATED each build by cmake/gen_version.cmake -- do not edit.\n\
#define X3_GIT_SHA \"${GIT_SHA}\"\n\
#define X3_GIT_HASH \"${GIT_HASH}\"\n\
#define X3_BUILD_NUMBER ${BUILD_NUMBER}\n\
#define X3_BUILD_DATE \"${BUILD_DATE}\"\n\
#define X3_BUILD_TS \"${BUILD_TS}\"\n\
#define X3_VERSION_NUM \"${VERSION_NUM}\"\n\
#define X3_VERSION_STRING \"${VERSION_STRING}\"\n\
#define X3_VERSION_STR \"X3Native  ${VERSION_NUM}  ${GIT_SHA}  (built ${BUILD_TS} UTC)\"\n")

# Only rewrite if changed, so we don't force a needless relink when nothing moved.
if(EXISTS "${OUT}")
    file(READ "${OUT}" EXISTING)
else()
    set(EXISTING "")
endif()
if(NOT EXISTING STREQUAL CONTENT)
    file(WRITE "${OUT}" "${CONTENT}")
endif()
