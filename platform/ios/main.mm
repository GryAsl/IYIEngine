#import <Foundation/Foundation.h>
#include "IYI/Application.h"
#include <unistd.h>

namespace {
IYI::Application app;
bool initialized = false;
bool stopped = false;

void StopOnce()
{
    if (initialized && !stopped) {
        stopped = true;
        app.Shutdown();
    }
}
}

// ghera/raylib-iOS 6.2.1-iOS, README.md (release commit da98037): rcore_ios.c calls these
// C callbacks on the main thread; ready initializes raylib before invoking ios_ready().
extern "C" void ios_ready()
{
    if (initialized) return;
    @autoreleasepool {
        const char* resources = [[[NSBundle mainBundle] resourcePath] fileSystemRepresentation];
        if (resources) chdir(resources);
    }
    char executable[] = "IYIEngineMobile";
    char* argv[] = { executable, nullptr };
    initialized = app.Initialize(1, argv);
    if (!initialized) StopOnce();
}

extern "C" void ios_update()
{
    if (initialized && !stopped && !app.Frame()) StopOnce();
}

extern "C" void ios_destroy()
{
    StopOnce();
}
