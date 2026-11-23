# A native game's exe (docs/spec/project.md §2c) in a few lines. <game>/CMakeLists.txt (or <game>/src/):
#
#   cmake_minimum_required(VERSION 3.24)
#   project(COIN LANGUAGES C CXX)
#   include("${CMAKE_CURRENT_SOURCE_DIR}/../IYIEngine/cmake/iyi_game.cmake")
#   iyi_add_game(COIN main.cpp world.cpp ...)
#
# and build it with `IYIEngine\tools\build_game.bat <game dir>`. The engine source is built inside the
# game's own build tree (add_subdirectory), so the game picks its options there: -DIYI_WITH_PYTHON=OFF
# leaves the interpreter and every binding out (shipping), -DIYI_SHIPPING_BUILD=ON. The exe lands in
# <project>/bin/<name>.exe, where it finds its project.json one folder up.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(IYI_BUILD_TESTS OFF CACHE BOOL "Build the native engine regression suite (ctest)")
set(IYI_BUILD_TOOLS OFF CACHE BOOL "Build development and packaging tools")
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_BINARY_DIR}/IYIEngine")

# The project folder: the one holding project.json, the CMakeLists' own folder or its parent.
set(IYI_GAME_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${IYI_GAME_DIR}/project.json")
    cmake_path(GET IYI_GAME_DIR PARENT_PATH IYI_GAME_DIR)
endif()

set(IYI_GAME_BIN_DIR "${IYI_GAME_DIR}/bin" CACHE PATH "Where the exe lands (tools/release.py: its shipping tree)")

function(iyi_add_game name)
    add_executable(${name} ${ARGN})
    iyi_engine_exe(${name})
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${IYI_GAME_BIN_DIR}")
    # tools/release.py writes the game's name, version and icon here (Task Manager, Explorer).
    if(IYI_GENERATED_INCLUDE_DIR AND WIN32 AND EXISTS "${IYI_GENERATED_INCLUDE_DIR}/GameVersion.rc")
        target_sources(${name} PRIVATE "${IYI_GENERATED_INCLUDE_DIR}/GameVersion.rc")
    endif()
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /permissive- /utf-8 /MP)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()
