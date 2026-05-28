# ============================================================================
# X3Native — build-time version header generation (clean-room, no new deps).
#
# Runs `git rev-list --count HEAD` (BUILD) + `git rev-parse --short HEAD` (HASH)
# and configure_file()s engine/core/version.h.in -> <gen>/engine/core/version.h
# defining X3_VERSION_MAJOR/MINOR/BUILD, X3_VERSION_STRING ("0.3.00284"),
# X3_GIT_HASH, X3_VERSION_FULL ("0.3.00284 (c3c74e1)").
#
# Invoked TWO ways:
#   1. At configure time (include() from the root CMakeLists) so the header
#      exists before the first build.
#   2. At build time via `cmake -P` from a custom target, so HASH + BUILD stay
#      fresh whenever HEAD moves — configure_file only rewrites the header when
#      its contents actually change, so a full rebuild isn't forced each time.
#
# Required input vars (passed with -D when run via `cmake -P`, or already set
# when include()d): X3_SRC_DIR, X3_VERSION_MAJOR, X3_VERSION_MINOR,
# X3_VERSION_IN (path to version.h.in), X3_VERSION_OUT (path to generated header).
#
# Graceful fallback: if git is missing or this isn't a repo, BUILD=0 (00000) and
# HASH="nogit" so the build NEVER breaks.
# ============================================================================

find_package(Git QUIET)

set(X3_VERSION_BUILD 0)
set(X3_GIT_HASH "nogit")

if(GIT_FOUND)
    # Commit count -> BUILD. ERROR_QUIET + result check so a non-repo is harmless.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
        WORKING_DIRECTORY "${X3_SRC_DIR}"
        OUTPUT_VARIABLE _x3_count
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _x3_count_rc)
    if(_x3_count_rc EQUAL 0 AND _x3_count MATCHES "^[0-9]+$")
        set(X3_VERSION_BUILD "${_x3_count}")
    endif()

    # Short hash.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${X3_SRC_DIR}"
        OUTPUT_VARIABLE _x3_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _x3_hash_rc)
    if(_x3_hash_rc EQUAL 0 AND NOT _x3_hash STREQUAL "")
        set(X3_GIT_HASH "${_x3_hash}")
    endif()
endif()

# Zero-pad BUILD to 5 digits: "284" -> "00284", "0" -> "00000". Clamp at >=5
# digits (a 6-digit count just won't be padded; the regex test stays 5-digit
# until then — see docs/VERSIONING.md).
set(X3_VERSION_BUILD_STR "${X3_VERSION_BUILD}")
string(LENGTH "${X3_VERSION_BUILD_STR}" _x3_len)
while(_x3_len LESS 5)
    set(X3_VERSION_BUILD_STR "0${X3_VERSION_BUILD_STR}")
    string(LENGTH "${X3_VERSION_BUILD_STR}" _x3_len)
endwhile()

# Compose the canonical strings here so the .in template stays trivial.
set(X3_VERSION_STRING "${X3_VERSION_MAJOR}.${X3_VERSION_MINOR}.${X3_VERSION_BUILD_STR}")
set(X3_VERSION_FULL   "${X3_VERSION_STRING} (${X3_GIT_HASH})")

configure_file("${X3_VERSION_IN}" "${X3_VERSION_OUT}" @ONLY)

message(STATUS "X3 version: ${X3_VERSION_FULL}  -> ${X3_VERSION_OUT}")
