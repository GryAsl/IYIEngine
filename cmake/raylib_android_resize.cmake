# raylib's Android backend keeps the first window's size forever: a foldable moving the game from
# its cover screen to the main one (a new or resized window) kept the old aspect and touch mapping.
# This adds a sync of the window size on every window change. Idempotent; CMakeLists.txt runs it in
# raylib's source folder on every configure.
set(file "src/platforms/rcore_android.c")
file(READ "${file}" src)
if(src MATCHES "IYI_SYNC_WINDOW_SIZE")
    return()
endif()
set(sync [=[
// IYI patch (cmake/raylib_android_resize.cmake): follow the window's real size. A buffer geometry
// set earlier would hide it, so it goes back to the window's own size first.
#define IYI_SYNC_WINDOW_SIZE(viewport) do { \
    ANativeWindow *win_ = platform.app->window; \
    ANativeWindow_setBuffersGeometry(win_, 0, 0, ANativeWindow_getFormat(win_)); \
    int w_ = ANativeWindow_getWidth(win_), h_ = ANativeWindow_getHeight(win_); \
    if (w_ > 0 && h_ > 0 && (w_ != CORE.Window.display.width || h_ != CORE.Window.display.height)) { \
        CORE.Window.display.width = CORE.Window.screen.width = CORE.Window.render.width = CORE.Window.currentFbo.width = w_; \
        CORE.Window.display.height = CORE.Window.screen.height = CORE.Window.render.height = CORE.Window.currentFbo.height = h_; \
        CORE.Window.renderOffset.x = CORE.Window.renderOffset.y = 0; \
        CORE.Window.screenScale = MatrixIdentity(); \
        if (viewport) SetupViewport(w_, h_); \
    } } while (0)
]=])
set(rebind "                if (platform.contextRebindRequired)\n                {\n")
set(rebound "                    platform.contextRebindRequired = false;\n")
set(config "        case APP_CMD_CONFIG_CHANGED:\n        {\n")
string(FIND "${src}" "${rebind}" a)
string(FIND "${src}" "${rebound}" d)
string(FIND "${src}" "${config}" b)
set(callback "static void AndroidCommandCallback(struct android_app *app, int32_t cmd)
{")
string(FIND "${src}" "${callback}" c)
if(a LESS 0 OR b LESS 0 OR c LESS 0 OR d LESS 0)
    message(WARNING "raylib_android_resize: anchors not found, foldable screen changes stay unhandled")
    return()
endif()
# The size before raylib sizes the new window's buffer from render.width; the viewport once the context is back.
string(REPLACE "${rebind}" "${rebind}                    IYI_SYNC_WINDOW_SIZE(0);\n" src "${src}")
string(REPLACE "${rebound}" "${rebound}                    SetupViewport(CORE.Window.render.width, CORE.Window.render.height);\n" src "${src}")
string(REPLACE "${config}" "        case APP_CMD_WINDOW_RESIZED:\n        case APP_CMD_CONTENT_RECT_CHANGED:\n${config}            if (app->window != NULL && !platform.contextRebindRequired) IYI_SYNC_WINDOW_SIZE(1);\n" src "${src}")
string(REPLACE "${callback}" "${sync}${callback}" src "${src}")
file(WRITE "${file}" "${src}")
