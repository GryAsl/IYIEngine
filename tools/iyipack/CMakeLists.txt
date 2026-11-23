cmake_minimum_required(VERSION 3.24)
project(iyipack LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# When included via add_subdirectory from root CMakeLists,
# raylib and nlohmann_json targets are already defined.
# If built standalone, fetch them here (raylib: the engine's current version, SPEC §0.1).
if(NOT TARGET raylib)
    include(FetchContent)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG        6.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif()

if(NOT TARGET nlohmann_json)
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.12.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

set(IYIPACK_SOURCES
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/monocypher/monocypher.c
)

add_executable(iyipack ${IYIPACK_SOURCES})

target_include_directories(iyipack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/rres
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/monocypher
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(iyipack PRIVATE
    raylib
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(iyipack PRIVATE advapi32 bcrypt)
endif()

target_compile_definitions(iyipack PRIVATE _CRT_SECURE_NO_WARNINGS)

if(MSVC)
    target_compile_options(iyipack PRIVATE /W4 /permissive- /utf-8 /MP /wd4456)
    # Disable benign C-level conversion warning in monocypher.c
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/monocypher/monocypher.c
        PROPERTIES COMPILE_FLAGS "/wd4244"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(iyipack PRIVATE -Wall -Wextra -Wpedantic)
endif()
