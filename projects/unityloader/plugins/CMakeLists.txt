function(bogodroid_add_unityloader_plugin name)
    if(NOT ARGN)
        message(FATAL_ERROR "bogodroid_add_unityloader_plugin(${name}) needs at least one source file")
    endif()

    set(target "bogodroid_unityloader_plugin_${name}")
    add_library(${target} SHARED EXCLUDE_FROM_ALL ${ARGN})
    add_custom_target("plugin_${name}" DEPENDS ${target})
    target_include_directories(${target} PRIVATE ${PROJ_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE BOGODROID_PLUGIN_BUILD)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        PREFIX ""
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/unityloader.d"
    )
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
    set_property(GLOBAL APPEND PROPERTY BOGODROID_UNITYLOADER_PLUGIN_TARGETS ${target})
endfunction()

file(GLOB plugin_cmake_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/*/CMakeLists.txt")
foreach(plugin_cmake IN LISTS plugin_cmake_files)
    get_filename_component(plugin_dir "${plugin_cmake}" DIRECTORY)
    add_subdirectory("${plugin_dir}" EXCLUDE_FROM_ALL)
endforeach()

get_property(plugin_targets GLOBAL PROPERTY BOGODROID_UNITYLOADER_PLUGIN_TARGETS)
if(plugin_targets)
    add_custom_target(unityloader_plugins DEPENDS ${plugin_targets})
endif()
