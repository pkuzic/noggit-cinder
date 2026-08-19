find_package(Qt5Core REQUIRED)

# get absolute path to qmake, then use it to find windeployqt executable

get_target_property(_qmake_executable Qt5::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin_dir "${_qmake_executable}" DIRECTORY)

function(windeployqt target)

    if (NOT DEFINED CMAKE_BUILD_TYPE)
        message("Warning: CMAKE_BUILD_TYPE is not defined. Both Release and Debug libraries will be deployed")
    endif()

    # POST_BUILD step
    # - after build, we have a bin/lib for analyzing qt dependencies
    # - we run windeployqt on target and deploy Qt libs

    # debug configuration
    if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT DEFINED CMAKE_BUILD_TYPE)
        # see the release branch below for why PATH is reduced to the Qt bin dir
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E env "PATH=${_qt_bin_dir}"
                "${_qt_bin_dir}/windeployqt.exe"
                --verbose 1
                --debug
                --no-svg
                --no-angle
                --no-opengl-sw
                --compiler-runtime
                --no-system-d3d-compiler
                --no-quick-import
                --no-translations
                --no-virtualkeyboard
                --no-webkit2
                --no-qmltooling
                \"$<TARGET_FILE:${target}>\"
                COMMENT "Deploying Qt libraries using windeployqt for compilation target '${target}' ..."
                )

    endif()

    # release configuration
    if (CMAKE_BUILD_TYPE STREQUAL "Release"
        OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo"
        OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel"
        OR NOT DEFINED CMAKE_BUILD_TYPE)

        # run with PATH reduced to the Qt bin dir: windeployqt resolves the target's dependent
        # DLLs via PATH, and a stray/broken Qt5*.dll elsewhere on the system PATH (e.g. an old
        # PyQt install) makes it abort -- after which MSBuild cleans the step's outputs, leaving
        # the exe without any Qt DLLs at all.
        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E env "PATH=${_qt_bin_dir}"
                "${_qt_bin_dir}/windeployqt.exe"
                --verbose 1
                --release
                --no-svg
                --no-angle
                --no-opengl-sw
                --compiler-runtime
                --no-system-d3d-compiler
                --no-quick-import
                --no-translations
                --no-virtualkeyboard
                --no-webkit2
                --no-qmltooling
                \"$<TARGET_FILE:${target}>\"
                COMMENT "Deploying Qt libraries using windeployqt for compilation target '${target}' ..."
                )
    endif()

endfunction()