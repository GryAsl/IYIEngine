#pragma once
// Mobile path roots (docs/mobile/plan/01 B5): the one place that knows where an app container keeps
// writable data and read-only assets. Implemented in ProjectConfig.cpp; desktop never includes it.
#ifdef IYI_MOBILE
#include <filesystem>

namespace IYI {
// Writable, app-private: Android activity->internalDataPath (filesDir), iOS Application Support.
std::filesystem::path MobileDataRoot();
}

#ifdef IYI_PLATFORM_ANDROID
#include <vector>
struct AAssetManager;
namespace IYI {
// The APK's assets (game.rres, python/manifest.txt); valid once raylib's android_main has started.
AAssetManager* MobileAssets();
struct MobileInsets { int left = 0, top = 0, right = 0, bottom = 0; };
int AndroidRefreshRate();
MobileInsets AndroidCutoutInsets();
void AndroidSetFrameRate(int fps);
void AndroidSetBufferSize(int width, int height);
int AndroidImeHeight();          // soft keyboard height in window pixels (0 = hidden), from IYIActivity
float ImeFieldBottom();          // focused text field's bottom edge as a fraction of the UI height; < 0 = none
void AndroidShowKeyboard(bool show);
void AndroidTakeText(std::vector<int>& chars);
void AndroidInstallCallbacks();
bool AndroidTakeTap(float& x, float& y, bool consume);
int AndroidDensity();
bool AndroidResumed();           // activity resumed with a window (Android pauses before it reports focus loss)
}
#endif
#endif
