find_package(Git QUIET)

set(purple_build_info_header ${CMAKE_CURRENT_BINARY_DIR}/gen/purple/purple_build_info.h)

add_custom_target(purple_build_info
    COMMAND ${CMAKE_COMMAND}
        -DPURPLE_GIT=${GIT_EXECUTABLE}
        -DPURPLE_REPO=${CMAKE_SOURCE_DIR}
        -DPURPLE_CORE=${purple_core_loc}
        -DPURPLE_OUTPUT=${purple_build_info_header}
        -P ${CMAKE_CURRENT_LIST_DIR}/purple_build_info.cmake
    BYPRODUCTS ${purple_build_info_header}
    VERBATIM
)
add_dependencies(Telegram purple_build_info)

nice_target_sources(Telegram ${src_loc}
PRIVATE
    purple/purple_version.cpp
    purple/purple_version.h
)
