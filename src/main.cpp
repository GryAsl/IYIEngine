// Serves as the main executable entry point for the IYI Engine runtime (IYIEngine.exe, Python games).
// The engine is meant to stay simple (not a UE5 or a Unity): extend what exists before adding a
// system, and keep every part readable by one person ("Simple over complete").
// RunGame (Game.cpp) creates the Application, passes the command line (--project <path>
// [--no-hot-reload] ...) to Application::Initialize, runs the loop and shuts down. A native game's
// exe (SPEC §2c) calls the same RunGame with its Game object.

#include "IYI/Game.h"
#ifdef IYI_PLATFORM_ANDROID
#include <android_native_app_glue.h>
#include <cstdlib>
extern "C" android_app* GetAndroidApp();
#endif

int main(int argc, char* argv[])
{
    const int result = IYI::RunGame(argc, argv, nullptr);
#ifdef IYI_PLATFORM_ANDROID
    if (auto* app = GetAndroidApp()) ANativeActivity_finish(app->activity);
    std::exit(result);
#endif
    return result;
}
