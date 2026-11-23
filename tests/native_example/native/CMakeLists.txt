# Fixture build for the engine's project-scoped native modules (docs/spec/project.md §2b). Built by its own
# native/build.bat; the engine's CMakeLists.txt does not depend on it. pybind11 is used header-only
# from the copy CMake already fetched for the engine, so this adds no new download.
cmake_minimum_required(VERSION 3.24)
project(native_example LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The interpreter to build against. This machine's PATH python is 3.11, which must never be picked
# silently, so the root is explicit and the version is exact. The default is the python.org per-user
# install folder; pass -DNATIVE_EXAMPLE_PYTHON_ROOT=<root> for any other.
file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/Python/Python314" _native_example_default_python)
set(NATIVE_EXAMPLE_PYTHON_ROOT "${_native_example_default_python}"
    CACHE PATH "CPython 3.14 root (python.exe, include/, libs/). Exactly 3.14 is required.")
set(NATIVE_EXAMPLE_PYBIND11_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../build/ninja/_deps/pybind11-src/include"
    CACHE PATH "pybind11 include directory (header-only; no pybind11 target is needed)")

if(NOT EXISTS "${NATIVE_EXAMPLE_PYTHON_ROOT}/include/Python.h")
    message(FATAL_ERROR
        "CPython 3.14 not found at '${NATIVE_EXAMPLE_PYTHON_ROOT}'. Pass "
        "-DNATIVE_EXAMPLE_PYTHON_ROOT=<root> pointing at a CPython 3.14 install (the one on PATH may be a different version).")
endif()
if(NOT EXISTS "${NATIVE_EXAMPLE_PYBIND11_INCLUDE}/pybind11/pybind11.h")
    message(FATAL_ERROR
        "pybind11 headers not found at '${NATIVE_EXAMPLE_PYBIND11_INCLUDE}'. Build the engine once "
        "(build.bat) so CMake fetches pybind11, or pass -DNATIVE_EXAMPLE_PYBIND11_INCLUDE=<dir>.")
endif()

set(Python3_ROOT_DIR "${NATIVE_EXAMPLE_PYTHON_ROOT}")
set(Python3_FIND_STRATEGY LOCATION)
find_package(Python3 3.14 EXACT COMPONENTS Interpreter Development.Module QUIET)
if(NOT Python3_FOUND)
    message(FATAL_ERROR
        "CPython 3.14 (Development.Module) not found under '${NATIVE_EXAMPLE_PYTHON_ROOT}'. The fixture "
        "must build against the interpreter the engine embeds, not whichever one CMake finds first.")
endif()

add_library(native_example MODULE native_example.cpp bindings.cpp)
target_include_directories(native_example PRIVATE
    "${NATIVE_EXAMPLE_PYBIND11_INCLUDE}"
    "${Python3_INCLUDE_DIRS}")
target_link_libraries(native_example PRIVATE Python3::Module)

# Use the standard extension suffix ("...cp314-win_amd64.pyd") so Python's import machinery finds the
# module and tools/release.py can check its interpreter tag.
execute_process(COMMAND "${Python3_EXECUTABLE}" -c
    "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"
    OUTPUT_VARIABLE NATIVE_EXAMPLE_EXT_SUFFIX OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _ext_result)
if(NOT _ext_result EQUAL 0 OR NOT NATIVE_EXAMPLE_EXT_SUFFIX)
    message(FATAL_ERROR "Could not read EXT_SUFFIX from ${Python3_EXECUTABLE}")
endif()

set_target_properties(native_example PROPERTIES
    PREFIX ""
    SUFFIX "${NATIVE_EXAMPLE_EXT_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
