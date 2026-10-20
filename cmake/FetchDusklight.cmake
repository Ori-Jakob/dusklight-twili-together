# Fetches the Dusklight tree Twili-Together builds against.
#
#   DUSKLIGHT_VERSION     tag or commit to fetch
#   DUSKLIGHT_REPOSITORY  git remote to fetch it from
#   DUSKLIGHT_DIR         where the tree lives (default <source>/dusklight); point it at an
#                         existing checkout to build against that instead
#
# Twili-Together uses host services upstream Dusklight does not have yet, so the default remote
# is the fork that carries them.

include_guard(GLOBAL)

set(DUSKLIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dusklight"
        CACHE PATH "Path to the Dusklight source tree")
set(DUSKLIGHT_REPOSITORY "https://github.com/Ori-Jakob/dusklight.git"
        CACHE STRING "Dusklight git repository to fetch from")

# Written into checkouts this file manages; a checkout without it is left alone.
set(_twili_stamp "${DUSKLIGHT_DIR}/.twili-stamp")
set(_twili_want "${DUSKLIGHT_REPOSITORY} ${DUSKLIGHT_VERSION}")

if (EXISTS "${DUSKLIGHT_DIR}/sdk/CMakeLists.txt" AND NOT EXISTS "${_twili_stamp}")
    message(STATUS "Dusklight: using the checkout at ${DUSKLIGHT_DIR}")
    return()
endif ()

if (NOT DUSKLIGHT_VERSION)
    message(FATAL_ERROR "Dusklight: DUSKLIGHT_VERSION is not set")
endif ()

set(_twili_have "")
if (EXISTS "${_twili_stamp}")
    file(STRINGS "${_twili_stamp}" _twili_have LIMIT_COUNT 1)
endif ()

if (NOT _twili_have STREQUAL _twili_want)
    find_package(Git QUIET REQUIRED)
    function(_twili_git)
        execute_process(COMMAND "${GIT_EXECUTABLE}" ${ARGN}
                WORKING_DIRECTORY "${DUSKLIGHT_DIR}" RESULT_VARIABLE _result)
        if (NOT _result EQUAL 0)
            list(JOIN ARGN " " _text)
            message(FATAL_ERROR "Dusklight: 'git ${_text}' failed. Check DUSKLIGHT_VERSION "
                    "(${DUSKLIGHT_VERSION}) and DUSKLIGHT_REPOSITORY (${DUSKLIGHT_REPOSITORY}).")
        endif ()
    endfunction()

    message(STATUS "Dusklight: fetching ${DUSKLIGHT_VERSION} from ${DUSKLIGHT_REPOSITORY}")
    file(MAKE_DIRECTORY "${DUSKLIGHT_DIR}")
    if (NOT EXISTS "${DUSKLIGHT_DIR}/.git")
        _twili_git(init --quiet)
    endif ()
    # A shallow fetch by name works for commits too, unlike FetchContent's GIT_SHALLOW.
    _twili_git(fetch --depth 1 "${DUSKLIGHT_REPOSITORY}" "${DUSKLIGHT_VERSION}")
    _twili_git(-c advice.detachedHead=false checkout --force FETCH_HEAD)
    # A reused checkout keeps the submodule URL from its first init unless synced.
    _twili_git(submodule sync --quiet -- extern/aurora)
    _twili_git(submodule update --init --depth 1 extern/aurora)
    file(WRITE "${_twili_stamp}" "${_twili_want}\n")

    set(_twili_exclude "${DUSKLIGHT_DIR}/.git/info/exclude")
    file(APPEND "${_twili_exclude}" "/.twili-stamp\n")
endif ()

# A shallow checkout has no tags for `git describe`.
set(DUSK_VERSION_OVERRIDE "${DUSKLIGHT_VERSION}")
