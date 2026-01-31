# Minimal WrapOpenGL module that avoids linking legacy AGL on macOS.
# Mirrors Qt's FindWrapOpenGL but omits the AGL framework.

if(TARGET WrapOpenGL::WrapOpenGL)
    set(WrapOpenGL_FOUND ON)
    return()
endif()

set(WrapOpenGL_FOUND OFF)

find_package(OpenGL ${WrapOpenGL_FIND_VERSION})

if(OpenGL_FOUND)
    set(WrapOpenGL_FOUND ON)

    add_library(WrapOpenGL::WrapOpenGL INTERFACE IMPORTED)
    if(APPLE)
        # Use the OpenGL library path if available; skip AGL entirely.
        get_target_property(__opengl_fw_lib_path OpenGL::GL IMPORTED_LOCATION)
        if(__opengl_fw_lib_path)
            set(__opengl_fw_path "${__opengl_fw_lib_path}")
        endif()

        if(NOT __opengl_fw_path)
            set(__opengl_fw_path "-framework OpenGL")
        endif()

        target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE ${__opengl_fw_path})
    else()
        target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE OpenGL::GL)
    endif()
elseif(UNIX AND NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Integrity")
    # Allow OpenGL on headless Linux when GLX is unavailable.
    find_package(OpenGL ${WrapOpenGL_FIND_VERSION} COMPONENTS OpenGL)
    if(OpenGL_FOUND)
        set(WrapOpenGL_FOUND ON)
        add_library(WrapOpenGL::WrapOpenGL INTERFACE IMPORTED)
        target_link_libraries(WrapOpenGL::WrapOpenGL INTERFACE OpenGL::OpenGL)
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WrapOpenGL DEFAULT_MSG WrapOpenGL_FOUND)
