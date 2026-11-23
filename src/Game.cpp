// Runs a game: IYIEngine.exe (Python, no Game object) and every native game's exe (SPEC §2c) start
// here, so both go through the same Initialize, Run and Shutdown.
//
// Connected to:
// - main.cpp (IYIEngine.exe) and a native game's main() call RunGame.
// - Application owns the Game object and the registered scenes.

#include "IYI/Game.h"
#include "IYI/Application.h"
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
#include "IYI/ImageCompare.h"
#include "IYI/Trailer.h"

#include <string>
#endif

namespace IYI {

int RunGame(int argc, char* argv[], std::unique_ptr<Game> game)
{
#ifdef __EMSCRIPTEN__
    static Application app;   // outlives main(): the browser main loop keeps running it (Application::Run)
#else
    Application app;
#endif
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    if (argc == 3 && std::string(argv[1]) == "--compare-images") {   // no window, no project (ImageCompare.h)
        return CompareImagesCli(argv[2]);
    }
#endif
    if (!app.Initialize(argc, argv, std::move(game))) {
        return 1;
    }
    app.Run();
    app.Shutdown();
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    return TrailerExitCode();   // 0 unless a --trailer-shot run failed
#else
    return 0;
#endif
}

void RegisterScene(const std::string& name, std::function<void()> sceneStart)
{
    CurrentApp().RegisterScene(name, std::move(sceneStart));
}

ActorRegistry& SceneActors()
{
    return CurrentApp().GetScene().Actors;
}

} // namespace IYI
