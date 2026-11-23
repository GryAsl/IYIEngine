# Browser executables, included from the root CMakeLists.txt only when EMSCRIPTEN is set (after IYIEngineCore).
add_executable(web_gate2 "${CMAKE_CURRENT_LIST_DIR}/smoke/python/main.cpp" "${CMAKE_CURRENT_LIST_DIR}/web_compat/posix_spawn.c")
target_link_libraries(web_gate2 PRIVATE IYIEngineCore)
iyi_web_preload(web_gate2 "${CMAKE_CURRENT_LIST_DIR}/smoke/python/proj@/proj")

# Gate 3+: the real engine executable (src/main.cpp) with a game preloaded at /game, which project
# discovery finds as <exe dir>/game (the exe dir is the virtual FS root). IYI_WEB_GAME picks the game.
set(IYI_WEB_GAME "${CMAKE_CURRENT_LIST_DIR}/smoke/engine" CACHE PATH "game folder preloaded at /game")
set(IYI_WEB_EXCLUDE "" CACHE STRING "file_packager --exclude patterns for game files (web_build.py: git-ignored)")
add_executable(web_engine src/main.cpp "${CMAKE_CURRENT_LIST_DIR}/web_compat/posix_spawn.c")
target_link_libraries(web_engine PRIVATE IYIEngineCore)
iyi_web_preload(web_engine)  # the stdlib only: the game is packaged on its own below

# The game goes into its own package (game.data + game.js, emscripten's file_packager), remade whenever
# a game file changes, so a Python or asset change repacks in seconds and never relinks the .wasm. Only
# what a desktop release ships (tools/release.py content_files): project.json, scripts/, assets/.
# Big CJK fonts (assets/fonts/*CJK*) go into fonts_cjk.data, which shell.html fetches after the game
# has started; the game must cope with them arriving late (HR: game.ui.fonts).
# project.json "libraries" go in too, at the path they resolve to from /game (ProjectConfig), so
# "../IYIEngine/libraries/nav" lands at /IYIEngine/libraries/nav.
set(_roots "${IYI_WEB_GAME}@/game")
file(READ "${IYI_WEB_GAME}/project.json" _project_json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${IYI_WEB_GAME}/project.json")
# "desktop_libraries" (project.md §2) are left out, and out of the packed project.json's "libraries".
set(_desktop_only)
string(JSON _desktop_count ERROR_VARIABLE _no_desktop LENGTH "${_project_json}" desktop_libraries)
if(NOT _no_desktop AND _desktop_count GREATER 0)
    math(EXPR _last "${_desktop_count} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _rel GET "${_project_json}" desktop_libraries ${_i})
        list(APPEND _desktop_only "${_rel}")
    endforeach()
endif()
set(_kept_libs "[]")
string(JSON _lib_count ERROR_VARIABLE _no_libs LENGTH "${_project_json}" libraries)
if(NOT _no_libs AND _lib_count GREATER 0)
    math(EXPR _last "${_lib_count} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _rel GET "${_project_json}" libraries ${_i})
        if(_rel IN_LIST _desktop_only)
            continue()
        endif()
        string(JSON _kept_count LENGTH "${_kept_libs}")
        string(JSON _kept_libs SET "${_kept_libs}" ${_kept_count} "\"${_rel}\"")
        cmake_path(ABSOLUTE_PATH _rel BASE_DIRECTORY "${IYI_WEB_GAME}" NORMALIZE OUTPUT_VARIABLE _lib)
        cmake_path(ABSOLUTE_PATH _rel BASE_DIRECTORY "/game" NORMALIZE OUTPUT_VARIABLE _lib_dst)
        list(APPEND _roots "${_lib}@${_lib_dst}")
    endforeach()
endif()
set(_game_files "${IYI_WEB_GAME}/project.json")
if(_desktop_only)
    string(JSON _web_project SET "${_project_json}" libraries "${_kept_libs}")
    file(WRITE "${CMAKE_BINARY_DIR}/web_project.json" "${_web_project}")
    set(_game_args "${CMAKE_BINARY_DIR}/web_project.json@/game/project.json")
else()
    set(_game_args "${IYI_WEB_GAME}/project.json@/game/project.json")
endif()
foreach(_root IN LISTS _roots)
    string(REPLACE "@" ";" _root "${_root}")
    list(GET _root 0 _src)
    list(GET _root 1 _dst)
    foreach(_sub scripts assets)
        if(EXISTS "${_src}/${_sub}")
            file(GLOB_RECURSE _files CONFIGURE_DEPENDS "${_src}/${_sub}/*")
            list(APPEND _game_files ${_files})
            list(APPEND _game_args "${_src}/${_sub}@${_dst}/${_sub}")
        endif()
    endforeach()
endforeach()
list(FILTER _game_files EXCLUDE REGEX "/__pycache__/|\.pyc$")
set(_cjk_fonts ${_game_files})
list(FILTER _cjk_fonts INCLUDE REGEX "/assets/fonts/[^/]*CJK[^/]*\.(otf|ttf)$")
if(CMAKE_HOST_WIN32)
    set(_packager "${EMSCRIPTEN_ROOT_PATH}/tools/file_packager.bat")
else()
    set(_packager "${EMSCRIPTEN_ROOT_PATH}/tools/file_packager")
endif()
set(_web_outputs "${CMAKE_BINARY_DIR}/game.data" "${CMAKE_BINARY_DIR}/game.js")
add_custom_command(OUTPUT ${_web_outputs}
    COMMAND "${_packager}" game.data --js-output=game.js --preload ${_game_args}
            "--exclude" "*/__pycache__*" "*.pyc" "*CJK*.otf" "*CJK*.ttf" ${IYI_WEB_EXCLUDE}
    DEPENDS ${_game_files} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" COMMENT "Packaging the game (game.data)" VERBATIM)
set(_font_args)
foreach(_f IN LISTS _cjk_fonts)
    file(RELATIVE_PATH _rel "${IYI_WEB_GAME}" "${_f}")
    list(APPEND _font_args "${_f}@/game/${_rel}")
endforeach()
if(_font_args)
    list(APPEND _web_outputs "${CMAKE_BINARY_DIR}/fonts_cjk.data" "${CMAKE_BINARY_DIR}/fonts_cjk.js")
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/fonts_cjk.data" "${CMAKE_BINARY_DIR}/fonts_cjk.js"
        COMMAND "${_packager}" fonts_cjk.data --js-output=fonts_cjk.js --preload ${_font_args}
        DEPENDS ${_cjk_fonts} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" COMMENT "Packaging CJK fonts (fonts_cjk.data)" VERBATIM)
else()
    file(REMOVE "${CMAKE_BINARY_DIR}/fonts_cjk.data" "${CMAKE_BINARY_DIR}/fonts_cjk.js")  # a game without them
endif()
add_custom_target(web_game_data ALL DEPENDS ${_web_outputs})
add_dependencies(web_engine web_game_data)
target_link_options(web_engine PRIVATE -lidbfs.js "SHELL:--pre-js \"${CMAKE_CURRENT_LIST_DIR}/persist.js\"")
set_property(TARGET web_engine APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/persist.js")
target_link_options(web_engine PRIVATE "SHELL:--shell-file \"${CMAKE_CURRENT_LIST_DIR}/shell.html\"")
set_property(TARGET web_engine APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shell.html")
# Without pthreads miniaudio plays through a ScriptProcessorNode on the main thread, whose default
# ~33 ms period crackles whenever one frame runs longer; 4096 frames (~85 ms) rides out a slow frame.
target_compile_definitions(raylib PRIVATE AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES=4096)
# The desktop executable is web_engine here (shim + preload); keep the plain one out of "all".
set_target_properties(IYIEngine PROPERTIES EXCLUDE_FROM_ALL ON)
