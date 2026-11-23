# A game's project-scoped native module (docs/spec/project.md §2b) in a few lines. <game>/native/CMakeLists.txt:
#
#   cmake_minimum_required(VERSION 3.24)
#   project(coin_native LANGUAGES CXX)
#   include("${CMAKE_CURRENT_SOURCE_DIR}/../../IYIEngine/cmake/iyi_native.cmake")
#   iyi_add_native_module(coin_native combat.cpp bindings.cpp)
#
# and build it with `..\..\IYIEngine\tools\build_native.bat` from <game>/native (or that script with the
# folder as its argument). The module lands in <game>/native/bin with the interpreter's extension suffix.
#
# It builds against the engine's own Release tree (build.bat first): the same CPython the engine embeds
# (read from that tree's cache, never whichever Python is on PATH), its pybind11 headers, and its
# IYIEngineSim + raylib static libraries, so the module's C++ calls the engine's plain C++ (SightGrid,
# NavPlanner, raymath, raylib's collision functions) directly. Window, drawing, input, audio and GPU
# functions of raylib are not usable there: their state lives in the engine executable's copy.
#
# Nothing here is used by the engine's own build; a game without a native module never reads it.

set(IYI_ENGINE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE PATH "IYIEngine repository root")
set(IYI_ENGINE_BUILD "${IYI_ENGINE_DIR}/build/ninja" CACHE PATH "The engine's Release build tree (build.bat)")

if(NOT EXISTS "${IYI_ENGINE_BUILD}/CMakeCache.txt" OR NOT EXISTS "${IYI_ENGINE_BUILD}/IYIEngineSim.lib")
    message(FATAL_ERROR "Engine build not found at '${IYI_ENGINE_BUILD}'. Run IYIEngine\\build.bat first, or pass -DIYI_ENGINE_BUILD=<tree>.")
endif()
# The engine tree's own settings, straight from its cache (internal entries included).
foreach(_iyi_key _Python3_EXECUTABLE IYI_PYTHON_VERSION CMAKE_BUILD_TYPE)
    file(STRINGS "${IYI_ENGINE_BUILD}/CMakeCache.txt" _iyi_line REGEX "^${_iyi_key}:[A-Z]+=")
    string(REGEX REPLACE "^[^=]*=" "" IYI_ENGINE_${_iyi_key} "${_iyi_line}")
endforeach()
if(NOT IYI_ENGINE_CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "The engine tree '${IYI_ENGINE_BUILD}' is '${IYI_ENGINE_CMAKE_BUILD_TYPE}'; native modules link a Release tree (same C runtime).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)

set(Python3_EXECUTABLE "${IYI_ENGINE__Python3_EXECUTABLE}")
find_package(Python3 ${IYI_ENGINE_IYI_PYTHON_VERSION} EXACT COMPONENTS Interpreter Development.Module REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sysconfig; print(sysconfig.get_config_var('EXT_SUFFIX'))"
    OUTPUT_VARIABLE IYI_NATIVE_EXT_SUFFIX OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _iyi_ext_result)
if(NOT _iyi_ext_result EQUAL 0 OR NOT IYI_NATIVE_EXT_SUFFIX)
    message(FATAL_ERROR "Could not read EXT_SUFFIX from ${Python3_EXECUTABLE}")
endif()

function(iyi_add_native_module name)
    add_library(${name} MODULE ${ARGN})
    target_include_directories(${name} PRIVATE
        "${IYI_ENGINE_DIR}/include"
        "${IYI_ENGINE_BUILD}/_deps/raylib-src/src"
        "${IYI_ENGINE_BUILD}/_deps/pybind11-src/include"
        "${Python3_INCLUDE_DIRS}")
    target_link_libraries(${name} PRIVATE Python3::Module
        "${IYI_ENGINE_BUILD}/IYIEngineSim.lib"
        "${IYI_ENGINE_BUILD}/_deps/raylib-build/raylib/raylib.lib")
    if(WIN32)
        target_link_libraries(${name} PRIVATE winmm gdi32 opengl32 user32 shell32)   # what raylib.lib's objects import
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8)
    endif()
    set_target_properties(${name} PROPERTIES PREFIX "" SUFFIX "${IYI_NATIVE_EXT_SUFFIX}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")
endfunction()
