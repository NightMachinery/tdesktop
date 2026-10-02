function(purple_head dir result)
    set(${result} "unknown" PARENT_SCOPE)
    if ("${PURPLE_GIT}" STREQUAL "" OR NOT EXISTS "${dir}/.git")
        return()
    endif()
    execute_process(
        COMMAND "${PURPLE_GIT}" -C "${dir}" rev-parse --short HEAD
        RESULT_VARIABLE code
        OUTPUT_VARIABLE head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if ("${code}" STREQUAL "0" AND NOT "${head}" STREQUAL "")
        set(${result} "${head}" PARENT_SCOPE)
    endif()
endfunction()

purple_head("${PURPLE_REPO}" desktop_commit)
purple_head("${PURPLE_CORE}" core_commit)

set(desktop_dirty "false")
if (NOT "${desktop_commit}" STREQUAL "unknown")
    execute_process(
        COMMAND "${PURPLE_GIT}" --no-optional-locks -C "${PURPLE_REPO}"
            status --porcelain --untracked-files=no --ignore-submodules=all
        RESULT_VARIABLE code
        OUTPUT_VARIABLE changes
        ERROR_QUIET)
    if ("${code}" STREQUAL "0" AND NOT "${changes}" STREQUAL "")
        set(desktop_dirty "true")
    endif()
endif()

set(content "#pragma once

namespace Purple::BuildInfo {

inline constexpr auto kDesktopCommit = \"${desktop_commit}\";
inline constexpr auto kDesktopDirty = ${desktop_dirty};
inline constexpr auto kCoreCommit = \"${core_commit}\";

} // namespace Purple::BuildInfo
")

set(current "")
if (EXISTS "${PURPLE_OUTPUT}")
    file(READ "${PURPLE_OUTPUT}" current)
endif()
if (NOT "${current}" STREQUAL "${content}")
    file(WRITE "${PURPLE_OUTPUT}" "${content}")
endif()
