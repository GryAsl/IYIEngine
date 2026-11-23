# Web (Emscripten) build settings, included from the root CMakeLists.txt only when EMSCRIPTEN is set.
# Replaces the desktop CPython discovery with the prebuilt wasm CPython from python-wasm-sdk (README).
# Default: third_party/web/python-wasm (setup_toolchain.py); the IYI_WEB_PYTHON_SDK env var or -D overrides it.
if(DEFINED ENV{IYI_WEB_PYTHON_SDK})
    set(_iyi_wpy_default "$ENV{IYI_WEB_PYTHON_SDK}")
else()
    set(_iyi_wpy_default "${CMAKE_CURRENT_LIST_DIR}/../../third_party/web/python-wasm")
endif()
set(IYI_WEB_PYTHON_SDK "${_iyi_wpy_default}" CACHE PATH "python-wasm-sdk extract root")
set(_iyi_wpy "${IYI_WEB_PYTHON_SDK}/devices/emsdk/usr")
if(NOT EXISTS "${_iyi_wpy}/lib/libpython${IYI_PYTHON_VERSION}.a")
    message(FATAL_ERROR "web: no libpython${IYI_PYTHON_VERSION}.a under ${_iyi_wpy}/lib; set IYI_WEB_PYTHON_SDK or run tools/web/setup_toolchain.py")
endif()

set(PLATFORM Web CACHE STRING "" FORCE)
set(GRAPHICS GRAPHICS_API_OPENGL_ES3 CACHE STRING "" FORCE)
set(PYBIND11_NOPYTHON ON CACHE BOOL "" FORCE)
# pybind11 needs C++ exceptions; native wasm exceptions, on every C++ object and the link.
add_compile_options(-fwasm-exceptions)
add_link_options(-fwasm-exceptions)

add_library(Python3::Python INTERFACE IMPORTED)
target_include_directories(Python3::Python INTERFACE "${_iyi_wpy}/include/python${IYI_PYTHON_VERSION}")
# Builtin modules of this libpython need these (sysconfig MODLIBS).
target_link_libraries(Python3::Python INTERFACE
    "${_iyi_wpy}/lib/libpython${IYI_PYTHON_VERSION}.a"
    "${IYI_WEB_PYTHON_SDK}/prebuilt/emsdk/lib_hacl${IYI_PYTHON_VERSION}.a"
    "${IYI_WEB_PYTHON_SDK}/prebuilt/emsdk/libexpat${IYI_PYTHON_VERSION}.a"
    "${_iyi_wpy}/lib/libmpdec.a" "${_iyi_wpy}/lib/libzstd.a" "${_iyi_wpy}/lib/libffi.a"
    "${_iyi_wpy}/lib/libssl.a" "${_iyi_wpy}/lib/libcrypto.a")
target_link_options(Python3::Python INTERFACE -sUSE_ZLIB=1 -sUSE_BZIP2=1 -sUSE_SQLITE3=1)

# The stdlib is preloaded to /python/lib/pythonX.Y in the virtual FS (iyi_web_preload below).
set(IYI_PYTHON_HOME_DIR "/python")
set(_iyi_copy_py_dll OFF)
set(IYI_WEB_STDLIB "${_iyi_wpy}/lib/python${IYI_PYTHON_VERSION}")

# Link flags shared by every browser executable.
set(IYI_WEB_LINK_OPTIONS -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=5MB -sFORCE_FILESYSTEM=1)

# Preload the stdlib (minus the static lib, its test suite and GUI tools) plus extra "<src>@<dst>" pairs.
function(iyi_web_preload target)
    # SHELL: keeps CMake from de-duplicating the repeated --preload-file flag.
    set(_opts "SHELL:--preload-file \"${IYI_WEB_STDLIB}@/python/lib/python${IYI_PYTHON_VERSION}\""
        "--exclude-file=*/config-${IYI_PYTHON_VERSION}-wasm32-emscripten*" "--exclude-file=*/ensurepip*"
        "--exclude-file=*/__pycache__*" "--exclude-file=*/idlelib*" "--exclude-file=*/tkinter*"
        "--exclude-file=*/turtledemo*" "--exclude-file=*/python${IYI_PYTHON_VERSION}/test/*")
    foreach(_p IN LISTS ARGN)
        list(APPEND _opts "SHELL:--preload-file \"${_p}\"")
    endforeach()
    target_link_options(${target} PRIVATE ${IYI_WEB_LINK_OPTIONS} ${_opts})
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
endfunction()
