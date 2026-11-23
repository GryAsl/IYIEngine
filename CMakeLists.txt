cmake_minimum_required(VERSION 3.24)
project(IYIEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Pinned dependency versions and Python path as cache variables
# raylib: the default is the current version the engine is developed against (now 6.2; it will be bumped again,
# steps in SPEC §0.1). Older versions stay as legacy options (now 5.5: build.bat rl55 -> build/ninja-rl55); new
# code may use current-only API and does not have to build on them.
set(IYI_RAYLIB_VERSION "6.2" CACHE STRING "raylib version: 6.2 (current) or 5.5 (legacy)")
set_property(CACHE IYI_RAYLIB_VERSION PROPERTY STRINGS 6.2 5.5)
message(STATUS "IYIEngine: raylib ${IYI_RAYLIB_VERSION}")
set(IYI_PYBIND11_VERSION "v3.1.0" CACHE STRING "pybind11 version")
set(IYI_JSON_VERSION "v3.12.0" CACHE STRING "nlohmann/json version")
set(IYI_ENET_VERSION "v1.3.18" CACHE STRING "ENet version (libraries/net only)")

# Embedded CPython: 3.14 by default, 3.11 kept as the legacy option. One build tree links one version,
# so give each version its own build dir (build.bat py311 -> build/ninja-py311).
set(IYI_PYTHON_VERSION "3.14" CACHE STRING "Embedded CPython version: 3.14 (default) or 3.11 (legacy)")
set_property(CACHE IYI_PYTHON_VERSION PROPERTY STRINGS 3.14 3.11)
if(NOT IYI_PYTHON_VERSION MATCHES "^3[.](14|11)$")
    message(FATAL_ERROR "IYI_PYTHON_VERSION must be 3.14 or 3.11, got '${IYI_PYTHON_VERSION}'")
endif()
string(REPLACE "." "" _iyi_py_tag "${IYI_PYTHON_VERSION}")
# Empty = let find_package(Python3) locate an install of that version (PATH, registry, system). Pass
# -DIYI_PYTHON_ROOT=<dir> to force a specific CPython root.
set(IYI_PYTHON_ROOT "" CACHE PATH "CPython root directory (optional)")

set(IYI_GENERATED_INCLUDE_DIR "" CACHE PATH "Directory containing generated release headers (e.g. ReleaseSecret.h)")
set(IYI_STEAMWORKS_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../steamworks_sdk_165/sdk" CACHE PATH "Path to Steamworks SDK")
# Steamworks: AUTO links it when the SDK's library for this platform exists, ON requires it (configure fails
# if missing), OFF never uses it.
set(IYI_STEAM "AUTO" CACHE STRING "Steamworks: AUTO, ON or OFF")
option(IYI_SHIPPING_BUILD "Build for shipping distribution" OFF)
option(IYI_BUILD_TOOLS "Build development and packaging tools" ON)
option(IYI_BUILD_TESTS "Build the native engine regression suite (ctest)" ON)
# OFF: no embedded CPython, no bindings, no pybind11 (a native game, SPEC §2c); IYIEngine.exe needs ON.
option(IYI_WITH_PYTHON "Embed CPython and the engine module (Python games)" ON)
# Multiplayer (libraries/net, SPEC §25): AUTO is ON when libraries/net is in the tree, ON requires it, OFF
# leaves out its C++, ENet and engine.net. tools/release.py passes ON only for a game that lists the library.
set(IYI_NET "AUTO" CACHE STRING "Multiplayer library: AUTO, ON or OFF")

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(IYI_PLATFORM_ANDROID ON)
    set(IYI_MOBILE ON)
    set(PLATFORM Android CACHE STRING "raylib platform" FORCE)
    # GLES 3.0 context (raylib's Android default is ES 2.0); StackLighting and the shader rewrite need ES 3.
    set(OPENGL_VERSION "ES 3.0" CACHE STRING "raylib OpenGL version" FORCE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IYI_PLATFORM_IOS ON)
    set(IYI_MOBILE ON)
endif()
if(IYI_MOBILE)
    set(IYI_STEAM OFF CACHE STRING "Steamworks" FORCE)
    set(IYI_BUILD_TOOLS OFF)
    set(IYI_BUILD_TESTS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(IYI_PYTHON_TARGET_ROOT "" CACHE PATH "Target CPython prefix")
    set(IYI_IOS_RAYLIB_XCFRAMEWORK "" CACHE PATH "iOS raylib xcframework")
    set(IYI_IOS_EGL_XCFRAMEWORK "" CACHE PATH "iOS EGL xcframework")
    set(IYI_IOS_GLESV2_XCFRAMEWORK "" CACHE PATH "iOS GLESv2 xcframework")
    if(IYI_PLATFORM_IOS)
        set(IYI_IOS_PYTHON_XCFRAMEWORK "" CACHE PATH "CPython iOS Python.xcframework")
        set(IYI_IOS_PYTHON_STDLIB "" CACHE PATH "CPython iOS stdlib root (contains lib/python3.14)")
        set(IYI_IOS_GAME_RRES "" CACHE FILEPATH "Packaged game.rres")
        foreach(_fw IYI_IOS_RAYLIB_XCFRAMEWORK IYI_IOS_EGL_XCFRAMEWORK IYI_IOS_GLESV2_XCFRAMEWORK IYI_IOS_PYTHON_XCFRAMEWORK)
            if(NOT EXISTS "${${_fw}}")
                message(FATAL_ERROR "${_fw} must name an existing xcframework")
            endif()
        endforeach()
        if(NOT EXISTS "${IYI_IOS_PYTHON_STDLIB}/lib/python3.14" OR NOT EXISTS "${IYI_IOS_GAME_RRES}")
            message(FATAL_ERROR "Set IYI_IOS_PYTHON_STDLIB (with lib/python3.14) and IYI_IOS_GAME_RRES")
        endif()
    endif()
endif()
if(IYI_MOBILE OR EMSCRIPTEN)
    set(IYI_NET OFF CACHE STRING "Multiplayer library" FORCE)   # desktop only in v1 (SPEC §25.2)
endif()
set(IYI_NET_ON OFF)
if(IYI_NET STREQUAL "ON" OR (IYI_NET STREQUAL "AUTO" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/src/Net.h"))
    set(IYI_NET_ON ON)
endif()
message(STATUS "IYIEngine: multiplayer (libraries/net) ${IYI_NET_ON}")
include(FetchContent)

# raylib
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        ${IYI_RAYLIB_VERSION}
    GIT_SHALLOW    TRUE
)

# pybind11
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        ${IYI_PYBIND11_VERSION}
    GIT_SHALLOW    TRUE
)

# nlohmann/json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${IYI_JSON_VERSION}
    GIT_SHALLOW    TRUE
)

if(IYI_MOBILE AND IYI_WITH_PYTHON)
    if(IYI_PLATFORM_IOS)
        set(_iyi_ios_python_headers "${IYI_IOS_PYTHON_XCFRAMEWORK}/ios-arm64/Python.framework/Headers")
        if(NOT EXISTS "${_iyi_ios_python_headers}/Python.h")
            message(FATAL_ERROR "Python.xcframework must contain ios-arm64/Python.framework/Headers/Python.h")
        endif()
        add_library(Python3::Python INTERFACE IMPORTED)
        set_target_properties(Python3::Python PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_iyi_ios_python_headers}")
        target_link_libraries(Python3::Python INTERFACE "${IYI_IOS_PYTHON_XCFRAMEWORK}")
    else()
        set(_iyi_py_lib "${IYI_PYTHON_TARGET_ROOT}/lib/libpython3.14.so")
        file(REAL_PATH "${IYI_PYTHON_TARGET_ROOT}" _iyi_py_root)
        file(REAL_PATH "${_iyi_py_lib}" _iyi_py_real)
        cmake_path(IS_PREFIX _iyi_py_root "${_iyi_py_real}" NORMALIZE _iyi_py_under_root)
        if(NOT IYI_PYTHON_TARGET_ROOT OR NOT EXISTS "${_iyi_py_lib}" OR NOT _iyi_py_under_root OR NOT EXISTS "${IYI_PYTHON_TARGET_ROOT}/include/python3.14/Python.h")
            message(FATAL_ERROR "Target Python headers and lib/libpython3.14.so must exist under IYI_PYTHON_TARGET_ROOT")
        endif()
        add_library(Python3::Python SHARED IMPORTED)
        set_target_properties(Python3::Python PROPERTIES IMPORTED_LOCATION "${_iyi_py_real}" INTERFACE_INCLUDE_DIRECTORIES "${_iyi_py_root}/include/python3.14")
    endif()
    set(PYBIND11_NOPYTHON ON CACHE BOOL "" FORCE)
    set(IYI_PYTHON_HOME_DIR "")
elseif(EMSCRIPTEN)
    include(tools/web/web.cmake)
elseif(IYI_WITH_PYTHON)
# CPython ${IYI_PYTHON_VERSION} embedded. Resolved BEFORE pybind11 so both use the same interpreter.
if(IYI_PYTHON_ROOT)
    set(Python3_ROOT_DIR "${IYI_PYTHON_ROOT}")
    set(Python3_FIND_STRATEGY LOCATION)
endif()
find_package(Python3 ${IYI_PYTHON_VERSION} EXACT COMPONENTS Interpreter Development.Embed QUIET)
if(NOT Python3_FOUND)
    message(FATAL_ERROR
        "IYIEngine needs CPython ${IYI_PYTHON_VERSION} (exactly) with development files "
        "(python${_iyi_py_tag}.lib / libpython${IYI_PYTHON_VERSION} and headers). Install it, or point CMake at it: "
        "-DIYI_PYTHON_ROOT=<path to the Python ${IYI_PYTHON_VERSION} root>. -DIYI_PYTHON_VERSION=3.11 builds the legacy version.")
endif()
if(IYI_PYTHON_ROOT)
    # An explicit root is a requirement, not a hint: refuse to silently use a different interpreter.
    cmake_path(IS_PREFIX IYI_PYTHON_ROOT "${Python3_EXECUTABLE}" NORMALIZE _iyi_py_under_root)
    if(NOT _iyi_py_under_root)
        message(FATAL_ERROR "IYI_PYTHON_ROOT='${IYI_PYTHON_ROOT}' was given but CMake resolved ${Python3_EXECUTABLE}. "
                            "Point IYI_PYTHON_ROOT at a CPython ${IYI_PYTHON_VERSION} root (the folder containing python.exe / bin/python${IYI_PYTHON_VERSION}).")
    endif()
endif()
# pybind11 (COMPAT mode) must bind against the same interpreter.
set(PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
set(Python_EXECUTABLE "${Python3_EXECUTABLE}")

# The embedded interpreter's home is the installation prefix, not the directory holding the executable.
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
    OUTPUT_VARIABLE IYI_PYTHON_HOME_DIR OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _iyi_py_rc)
if(NOT _iyi_py_rc EQUAL 0 OR NOT IYI_PYTHON_HOME_DIR)
    get_filename_component(IYI_PYTHON_HOME_DIR "${Python3_EXECUTABLE}" DIRECTORY)
endif()
file(TO_CMAKE_PATH "${IYI_PYTHON_HOME_DIR}" IYI_PYTHON_HOME_DIR)
# Windows development builds copy the linked python3XX.dll beside the exe and the tests (below), so each build
# tree (build/ninja = 3.14, build/ninja-py311 = 3.11) starts whichever Python is on PATH. Shipping bundles its own.
set(_iyi_py_dll "${IYI_PYTHON_HOME_DIR}/python${_iyi_py_tag}.dll")
set(_iyi_copy_py_dll OFF)
if(WIN32 AND NOT IYI_SHIPPING_BUILD AND EXISTS "${_iyi_py_dll}")
    set(_iyi_copy_py_dll ON)
endif()

endif()

# Shipping on GCC/Clang: keep source/build-tree paths (home dir, runner name) out of the binaries, deps included.
if(IYI_SHIPPING_BUILD AND NOT MSVC)
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=.")
endif()

if(IYI_PLATFORM_IOS)
    add_library(raylib INTERFACE)
    target_link_libraries(raylib INTERFACE "${IYI_IOS_RAYLIB_XCFRAMEWORK}" "${IYI_IOS_EGL_XCFRAMEWORK}" "${IYI_IOS_GLESV2_XCFRAMEWORK}")
    FetchContent_MakeAvailable(nlohmann_json)
else()
    FetchContent_MakeAvailable(raylib nlohmann_json)
    # Every configure (idempotent), so an existing build folder gets it too; the build then recompiles it.
    execute_process(COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/raylib_android_resize.cmake
                    WORKING_DIRECTORY ${raylib_SOURCE_DIR})
endif()
if(IYI_WITH_PYTHON)
    FetchContent_MakeAvailable(pybind11)
endif()

# raylib's own mitered joints for DrawSplineLinear (off in its default config): consecutive segments of a
# thick polyline meet without the notches a curve would otherwise show on its outer edge. A turn sharper
# than ~120 degrees makes a long spike, so such a corner is drawn as two polylines.
if(NOT IYI_PLATFORM_IOS)
    target_compile_definitions(raylib PRIVATE SUPPORT_SPLINE_MITERS=1)
endif()

if(IYI_BUILD_TOOLS)
    add_subdirectory(tools/iyipack)
endif()

set(SOURCES
    src/Api.cpp
    src/Application.cpp
    src/Game.cpp
    src/ProjectConfig.cpp
    src/ActorRegistry.cpp
    src/Scene.cpp
    src/AssetManager.cpp
    src/ResourceStore.cpp
    src/IYIUI.cpp
    src/IYIUIDraw.cpp
    src/IYIUIInput.cpp
    src/IYIUILayout.cpp
    src/IYIUITheme.cpp
    src/Ime.cpp
    src/IYIUITween.cpp
    src/IYIUIIcons.cpp
    src/LocalizationManager.cpp
    src/AudioManager.cpp
    src/Log.cpp
    src/Profiler.cpp
    src/NavApi.cpp
    src/AgentChannel.cpp
    src/CrashReport.cpp
    src/ImageCompare.cpp
    src/SpriteStack.cpp
    src/StackLighting.cpp
    src/Particles.cpp
    src/CellTerrain.cpp
    src/FramePacket.cpp
    src/FrameInput.cpp
    src/Video.cpp
)
# The engine libraries' runtime C++ (libraries/<lib>/src, included as "<lib>/src/X.h"); each library's
# Python module wraps it through src/LibraryBindings.cpp (docs/native-games-plan.md E7).
list(APPEND SOURCES libraries/sprite_stack/src/Daylight.cpp libraries/sprite_stack/src/Fx.cpp libraries/sprite_stack/src/Post.cpp)
list(APPEND SOURCES libraries/rts_select/src/Select.cpp libraries/profiler_overlay/src/Overlay.cpp)
list(APPEND SOURCES libraries/geo_world/src/World.cpp)
if(IYI_NET_ON)
    list(APPEND SOURCES libraries/net/src/Net.cpp libraries/net/src/NetSession.cpp libraries/net/src/NetLink.cpp
        libraries/net/src/NetSteam.cpp)
    if(IYI_WITH_PYTHON)
        list(APPEND SOURCES src/NetBindings.cpp)
    endif()
endif()
# The embedded interpreter and the engine module: every pybind11 file (rule 14).
if(IYI_WITH_PYTHON)
    list(APPEND SOURCES src/PythonRuntime.cpp src/EngineBindings.cpp src/RaylibBindings.cpp src/IYIUIBindings.cpp
        src/LocalizationBindings.cpp src/AudioBindings.cpp src/NavBindings.cpp
        src/NavSimBindings.cpp src/SightBindings.cpp src/LibraryBindings.cpp src/GeoWorldBindings.cpp src/VideoBindings.cpp)
endif()

list(APPEND SOURCES src/Steam.cpp)   # without IYI_STEAM (always on mobile) it is the no-Steam build
# engine.play: JNI to the Android shell on Android, the off values everywhere else.
list(APPEND SOURCES src/AndroidPlayServices.cpp)
if(IYI_WITH_PYTHON)
    list(APPEND SOURCES src/SteamBindings.cpp src/PlayBindings.cpp)
endif()

# IYITrailer (docs/IYITRAILER.md) is development-only: a shipping build never compiles it.
if(NOT IYI_SHIPPING_BUILD AND NOT IYI_MOBILE)
    list(APPEND SOURCES src/Trailer.cpp src/TrailerProcess.cpp)
    if(IYI_WITH_PYTHON)
        list(APPEND SOURCES src/TrailerBindings.cpp)
    endif()
endif()

# IYIEngineSim: the plain C++ simulation parts (raylib types allowed; no GL, no Python, no engine state).
# The engine links it like any of its code, and a game's native module links the same library to call
# them directly, without Python in between (docs/spec/project.md §2b, cmake/iyi_native.cmake).
add_library(IYIEngineSim STATIC src/Life.cpp src/Nav.cpp src/NavSim.cpp src/NavSimRoads.cpp src/NavSimUnits.cpp src/NavSimDrive.cpp src/PyExact.cpp src/Sight.cpp)
target_include_directories(IYIEngineSim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(IYIEngineSim PUBLIC raylib)

# Everything except main() lives in a static library so the native tests link the same code as the game.
add_library(IYIEngineCore STATIC ${SOURCES})

# An exe running the engine: IYIEngine.exe, the tests, a native game (cmake/iyi_game.cmake). A shipped
# game opens no console window beside it (its output is in the log file either way), and the DLLs the
# engine loads at runtime (IYI_RUNTIME_DLLS, filled below) are copied beside it.
function(iyi_engine_exe target)
    target_link_libraries(${target} PRIVATE IYIEngineCore)
    if(IYI_SHIPPING_BUILD AND WIN32)
        set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
        if(MSVC)
            target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
    if(IYI_SHIPPING_BUILD AND NOT WIN32)
        # Bundled libs sit beside the exe (Linux) or in Contents/Frameworks (.app): only that relative RPATH.
        if(APPLE)
            set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "@executable_path/../Frameworks")
        else()
            set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
        endif()
    endif()
    foreach(_dll IN LISTS IYI_RUNTIME_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
endfunction()

# tools/release.py writes the game's name and version here (GameVersion.rc: Task Manager, file Properties).
if(IYI_GENERATED_INCLUDE_DIR)
    target_include_directories(IYIEngineCore PRIVATE "${IYI_GENERATED_INCLUDE_DIR}")
    # The generated ReleaseSecret.h shadows the committed placeholder, and every release writes a new key:
    # ninja's header deps only know the placeholder, so name the real header or a stale key ships.
    if(IYI_SHIPPING_BUILD)
        set_source_files_properties(src/ResourceStore.cpp PROPERTIES
            OBJECT_DEPENDS "${IYI_GENERATED_INCLUDE_DIR}/IYI/ReleaseSecret.h")
    endif()
endif()

target_include_directories(IYIEngineCore PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/libraries"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rres"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/monocypher"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"   # pl_mpeg/pl_mpeg.h
)
if(IYI_PLATFORM_ANDROID)
    target_sources(IYIEngineCore PRIVATE src/AndroidPlatform.cpp)
    # android_native_app_glue.h (GetAndroidApp() in ProjectConfig.cpp); no GLFW on mobile.
    target_include_directories(IYIEngineCore PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")
    target_link_libraries(IYIEngineCore PUBLIC log android dl)
elseif(NOT IYI_MOBILE)
    # GLFW headers ship inside raylib's desktop platform layer; RaylibBindings.cpp needs them for
    # the monitor mode list, which raylib itself does not expose.
    target_include_directories(IYIEngineCore PUBLIC "${raylib_SOURCE_DIR}/src/external/glfw/include")
endif()

# Shipping resolves Python from the package only, so the build machine's path is never compiled in.
if(IYI_SHIPPING_BUILD)
    set(IYI_PYTHON_HOME_DIR "")
endif()
target_compile_definitions(IYIEngineCore PUBLIC
    IYI_PYTHON_HOME="${IYI_PYTHON_HOME_DIR}"
    $<$<BOOL:${IYI_MOBILE}>:IYI_MOBILE>
    $<$<BOOL:${IYI_PLATFORM_ANDROID}>:IYI_PLATFORM_ANDROID>
    $<$<BOOL:${IYI_PLATFORM_IOS}>:IYI_PLATFORM_IOS>
    _CRT_SECURE_NO_WARNINGS
    $<$<BOOL:${IYI_WITH_PYTHON}>:IYI_WITH_PYTHON>
)

if(IYI_SHIPPING_BUILD)
    target_compile_definitions(IYIEngineCore PUBLIC IYI_SHIPPING_BUILD)
endif()

target_link_libraries(IYIEngineCore PUBLIC IYIEngineSim raylib nlohmann_json::nlohmann_json)
if(IYI_NET_ON)
    # ENet's own CMakeLists is skipped (SOURCE_SUBDIR has none): its C files build here, warnings and all.
    FetchContent_Declare(enet GIT_REPOSITORY https://github.com/lsalzman/enet.git GIT_TAG ${IYI_ENET_VERSION}
        GIT_SHALLOW TRUE SOURCE_SUBDIR iyi-no-cmake)
    FetchContent_MakeAvailable(enet)
    add_library(iyi_enet STATIC ${enet_SOURCE_DIR}/callbacks.c ${enet_SOURCE_DIR}/compress.c ${enet_SOURCE_DIR}/host.c
        ${enet_SOURCE_DIR}/list.c ${enet_SOURCE_DIR}/packet.c ${enet_SOURCE_DIR}/peer.c ${enet_SOURCE_DIR}/protocol.c
        ${enet_SOURCE_DIR}/unix.c ${enet_SOURCE_DIR}/win32.c)
    target_include_directories(iyi_enet PUBLIC "${enet_SOURCE_DIR}/include")
    if(WIN32)
        target_link_libraries(iyi_enet PUBLIC ws2_32 winmm)
    else()
        target_compile_definitions(iyi_enet PRIVATE HAS_SOCKLEN_T=1 HAS_POLL=1 HAS_FCNTL=1 HAS_MSGHDR_FLAGS=1)
    endif()
    target_link_libraries(IYIEngineCore PUBLIC iyi_enet)
    target_compile_definitions(IYIEngineCore PUBLIC IYI_NET)
endif()
if(IYI_WITH_PYTHON)
    if(IYI_MOBILE)
        target_link_libraries(IYIEngineCore PUBLIC pybind11::headers Python3::Python)
    else()
        target_link_libraries(IYIEngineCore PUBLIC $<IF:$<BOOL:${EMSCRIPTEN}>,pybind11::headers,pybind11::embed> Python3::Python)
    endif()
endif()
if(WIN32)
    target_link_libraries(IYIEngineCore PUBLIC imm32 comctl32 dbghelp psapi shell32)
endif()

# Steam SDK binaries per platform: _iyi_steam_lib is linked, _iyi_steam_rt is the runtime copy shipped beside the exe.
set(_iyi_steam_bin "${IYI_STEAMWORKS_SDK}/redistributable_bin")
if(WIN32)
    set(_iyi_steam_lib "${_iyi_steam_bin}/win64/steam_api64.lib")
    set(_iyi_steam_rt  "${_iyi_steam_bin}/win64/steam_api64.dll")
elseif(APPLE)
    set(_iyi_steam_lib "${_iyi_steam_bin}/osx/libsteam_api.dylib")
    set(_iyi_steam_rt  "${_iyi_steam_lib}")
else()
    set(_iyi_steam_lib "${_iyi_steam_bin}/linux64/libsteam_api.so")
    set(_iyi_steam_rt  "${_iyi_steam_lib}")
endif()
set(_iyi_use_steam OFF)
if(NOT IYI_STEAM STREQUAL "OFF")
    if(EXISTS "${_iyi_steam_lib}")
        set(_iyi_use_steam ON)
    elseif(IYI_STEAM STREQUAL "ON")
        message(FATAL_ERROR "IYI_STEAM=ON but '${_iyi_steam_lib}' was not found. Set -DIYI_STEAMWORKS_SDK=<sdk dir> or use -DIYI_STEAM=OFF.")
    endif()
endif()

if(_iyi_use_steam)
    target_compile_definitions(IYIEngineCore PUBLIC IYI_STEAM)
    target_include_directories(IYIEngineCore PUBLIC
        "${IYI_STEAMWORKS_SDK}/public"
        "${IYI_STEAMWORKS_SDK}/public/steam"
    )
    target_link_libraries(IYIEngineCore PUBLIC "${_iyi_steam_lib}")
    list(APPEND IYI_RUNTIME_DLLS "${_iyi_steam_rt}")
endif()
if(_iyi_copy_py_dll)
    list(APPEND IYI_RUNTIME_DLLS "${_iyi_py_dll}")
endif()
if(NOT PROJECT_IS_TOP_LEVEL)
    set(IYI_RUNTIME_DLLS "${IYI_RUNTIME_DLLS}" PARENT_SCOPE)   # a native game's exe (iyi_game.cmake)
endif()

if(IYI_MOBILE)
    set_target_properties(IYIEngineCore IYIEngineSim PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    if(IYI_PLATFORM_IOS)
        include(platform/ios/CMakeLists.txt)
    else()
        add_library(iyigame SHARED src/main.cpp)
        target_link_libraries(iyigame PRIVATE IYIEngineCore)
        if(IYI_PLATFORM_ANDROID)
            target_link_options(iyigame PRIVATE "-uANativeActivity_onCreate")
            target_link_libraries(iyigame PRIVATE GLESv3)
        endif()
    endif()
    message(STATUS "IYI mobile: system=${CMAKE_SYSTEM_NAME} ABI=${ANDROID_ABI} API=${ANDROID_PLATFORM} python=${_iyi_py_lib} raylib=${PLATFORM} shipping=${IYI_SHIPPING_BUILD} steam=${IYI_STEAM}")
elseif(IYI_WITH_PYTHON)
    add_executable(IYIEngine src/main.cpp)
    iyi_engine_exe(IYIEngine)
    if(IYI_GENERATED_INCLUDE_DIR AND WIN32 AND EXISTS "${IYI_GENERATED_INCLUDE_DIR}/GameVersion.rc")
        target_sources(IYIEngine PRIVATE "${IYI_GENERATED_INCLUDE_DIR}/GameVersion.rc")
    endif()
endif()

if(MSVC)
    target_compile_options(IYIEngineCore PRIVATE /W4 /permissive- /utf-8 /MP)
    target_compile_options(IYIEngineSim PRIVATE /W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(IYIEngineCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(IYIEngineSim PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(IYI_BUILD_TESTS)
    enable_testing()
    add_executable(IYIEngineTests tests/engine_tests.cpp tests/ime_tests.cpp)
    if(IYI_NET_ON)
        target_sources(IYIEngineTests PRIVATE tests/net_tests.cpp)
    endif()
    iyi_engine_exe(IYIEngineTests)
    if(MSVC)
        target_compile_options(IYIEngineTests PRIVATE /W4 /permissive- /utf-8)
    endif()
    add_test(NAME engine_tests COMMAND IYIEngineTests)
endif()

if(EMSCRIPTEN)
    include(tools/web/targets.cmake)
endif()
