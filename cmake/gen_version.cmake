# Regenerates version_gen.h on EVERY build (driven by an always-run custom target).
# Stamps the git short hash + dirty flag + build timestamp so the window title
# tells you EXACTLY which build you are running. (Owner: "put a VERSION NUMBER in
# the TITLE" — the root cause of a night of debugging the wrong exe.)
execute_process(
    COMMAND git -C "${SRC}" rev-parse --short HEAD
    OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT GIT_HASH)
    set(GIT_HASH "nogit")
endif()

# Uncommitted changes? mark the build "+dirty" so a local edit is never mistaken
# for the committed hash.
execute_process(
    COMMAND git -C "${SRC}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE GIT_DIRTY OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_DIRTY)
    set(GIT_HASH "${GIT_HASH}+dirty")
endif()

string(TIMESTAMP BUILD_TS "%Y-%m-%d %H:%M" UTC)

set(CONTENT "#pragma once\n// AUTO-GENERATED each build by cmake/gen_version.cmake — do not edit.\n#define X3_GIT_HASH \"${GIT_HASH}\"\n#define X3_BUILD_TS \"${BUILD_TS}\"\n#define X3_VERSION_STR \"X3Native  ${GIT_HASH}  (built ${BUILD_TS} UTC)\"\n")

# Only rewrite if changed, so we don't force a needless relink when nothing moved.
if(EXISTS "${OUT}")
    file(READ "${OUT}" EXISTING)
else()
    set(EXISTING "")
endif()
if(NOT EXISTING STREQUAL CONTENT)
    file(WRITE "${OUT}" "${CONTENT}")
endif()
