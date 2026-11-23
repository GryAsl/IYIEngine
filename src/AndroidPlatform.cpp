#include "MobilePaths.h"
#include "AndroidJni.h"
#include "IYI/Log.h"
#include <android/input.h>
#include <android_native_app_glue.h>
#include <android/api-level.h>
#include <android/native_window.h>
#include <android/configuration.h>
#include <jni.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>

namespace {
std::mutex textMutex;
std::vector<int> textQueue;
void (*commandCallback)(android_app*, int32_t) = nullptr;
int32_t (*inputCallback)(android_app*, AInputEvent*) = nullptr;
bool unseenDown = false;
std::deque<std::pair<float, float>> taps;
float tapX = 0, tapY = 0;
int64_t downTime = 0;
int bufferW = 0, bufferH = 0;  // the window buffer size last set; 0 = raylib's own

void Command(android_app* app, int32_t cmd) {
    const char* name = nullptr;
    switch (cmd) {
    case APP_CMD_PAUSE: name = "PAUSE"; break;
    case APP_CMD_RESUME: name = "RESUME"; break;
    case APP_CMD_TERM_WINDOW: name = "TERM_WINDOW"; break;
    case APP_CMD_INIT_WINDOW: name = "INIT_WINDOW"; break;
    case APP_CMD_DESTROY: name = "DESTROY"; break;
    case APP_CMD_CONFIG_CHANGED: name = "CONFIG_CHANGED"; break;
    case APP_CMD_WINDOW_RESIZED: name = "WINDOW_RESIZED"; break;
    case APP_CMD_GAINED_FOCUS: name = "GAINED_FOCUS"; break;
    case APP_CMD_LOST_FOCUS: name = "LOST_FOCUS"; break;
    }
    if (name) IYI::LogWrite(IYI::LogLevel::Info, "android", std::string("APP_CMD_") + name);
    commandCallback(app, cmd);
    // raylib's rebind or its window-size sync (cmake/raylib_android_resize.cmake) set its own geometry: apply ours again
    if (cmd == APP_CMD_INIT_WINDOW || cmd == APP_CMD_WINDOW_RESIZED || cmd == APP_CMD_CONTENT_RECT_CHANGED ||
        cmd == APP_CMD_CONFIG_CHANGED)
        bufferW = bufferH = 0;
}
int32_t Input(android_app* app, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION &&
        (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN) {
        const int action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_DOWN) {
            unseenDown = true;
            downTime = AMotionEvent_getEventTime(event);
            tapX = AMotionEvent_getX(event, 0); tapY = AMotionEvent_getY(event, 0);
        } else if (action == AMOTION_EVENT_ACTION_UP) {
            if (unseenDown && AMotionEvent_getEventTime(event) - downTime <= 500000000) taps.emplace_back(tapX, tapY);
            unseenDown = false;
        } else if (action == AMOTION_EVENT_ACTION_CANCEL) unseenDown = false;
    }
    return inputCallback(app, event);
}
}

std::atomic<int> imeHeight{0};
extern "C" JNIEXPORT void JNICALL Java_com_iyi_engine_IYIActivity_nativeOnImeHeight(JNIEnv*, jclass, jint height) {
    imeHeight = height;
}

extern "C" JNIEXPORT void JNICALL Java_com_iyi_engine_IYIActivity_nativeOnText(JNIEnv* env, jclass, jstring text) {
    if (!text) return;
    const jchar* chars = env->GetStringChars(text, nullptr);
    if (!chars) return;
    const jsize length = env->GetStringLength(text);
    std::lock_guard<std::mutex> lock(textMutex);
    for (jsize i = 0; i < length; ++i) {
        int c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < length && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff)
            c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
        if (c && (c < 0xd800 || c > 0xdfff)) textQueue.push_back(c);
    }
    env->ReleaseStringChars(text, chars);
}

extern "C" struct android_app* GetAndroidApp(void);

namespace IYI {

void AndroidInstallCallbacks() {
    auto* app = GetAndroidApp();
    if (!app) return;
    commandCallback = app->onAppCmd; inputCallback = app->onInputEvent;
    app->onAppCmd = Command; app->onInputEvent = Input;
    IYI::LogWrite(IYI::LogLevel::Info, "android", "APP_CMD_INIT_WINDOW (initial window already created)");
}
bool AndroidTakeTap(float& x, float& y, bool consume) {
    unseenDown = false;
    if (!consume || taps.empty()) return false;
    x = taps.front().first; y = taps.front().second; taps.pop_front();
    return true;
}

void AndroidTakeText(std::vector<int>& chars) {
    std::lock_guard<std::mutex> lock(textMutex);
    chars.insert(chars.end(), textQueue.begin(), textQueue.end());
    textQueue.clear();
}

int AndroidImeHeight() { return imeHeight; }

bool AndroidResumed() {
    auto* app = GetAndroidApp();
    return app && app->window && app->activityState == APP_CMD_RESUME;
}

int AndroidDensity() {
    auto* app = GetAndroidApp();
    return app && app->config ? AConfiguration_getDensity(app->config) : 0;
}

void AndroidShowKeyboard(bool show) {
    Jni j;
    auto* app = GetAndroidApp();
    if (!j.env || !app || !app->activity) return;
    jobject activity = app->activity->clazz;
    jclass cls = j.env->GetObjectClass(activity);
    jmethodID method = cls ? j.env->GetMethodID(cls, "showKeyboard", "(Z)V") : nullptr;
    if (method) j.env->CallVoidMethod(activity, method, static_cast<jboolean>(show));
    if (cls) j.env->DeleteLocalRef(cls);
}
int AndroidRefreshRate() {
    static int cached = 0;
    if (cached) return cached;
    Jni j;
    auto* app = GetAndroidApp();
    if (!j.env || !app || !app->activity) return 0;
    JNIEnv* e = j.env;
    jobject window = CallObject(e, app->activity->clazz, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject display = CallObject(e, window, "getDefaultDisplay", "()Landroid/view/Display;");
    auto modes = static_cast<jobjectArray>(CallObject(e, display, "getSupportedModes", "()[Landroid/view/Display$Mode;"));
    float highest = 0;
    if (modes && !e->ExceptionCheck()) {
        const jsize count = e->GetArrayLength(modes);
        for (jsize i = 0; i < count; ++i) {
            jobject mode = e->GetObjectArrayElement(modes, i);
            jclass cls = e->GetObjectClass(mode);
            jmethodID method = e->GetMethodID(cls, "getRefreshRate", "()F");
            if (method) highest = std::max(highest, e->CallFloatMethod(mode, method));
            e->DeleteLocalRef(cls); e->DeleteLocalRef(mode);
            if (e->ExceptionCheck()) break;
        }
    }
    if (modes) e->DeleteLocalRef(modes);
    if (display) e->DeleteLocalRef(display);
    if (window) e->DeleteLocalRef(window);
    if (highest > 0) cached = static_cast<int>(std::lround(highest));
    return cached;
}

MobileInsets AndroidCutoutInsets() {
    MobileInsets result;
    if (android_get_device_api_level() < 28) return result;
    Jni j;
    auto* app = GetAndroidApp();
    if (!j.env || !app || !app->activity) return result;
    JNIEnv* e = j.env;
    jobject window = CallObject(e, app->activity->clazz, "getWindow", "()Landroid/view/Window;");
    jobject decor = CallObject(e, window, "getDecorView", "()Landroid/view/View;");
    jobject insets = CallObject(e, decor, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
    jobject cutout = CallObject(e, insets, "getDisplayCutout", "()Landroid/view/DisplayCutout;");
    if (cutout && !e->ExceptionCheck()) {
        jclass cls = e->GetObjectClass(cutout);
        const char* names[] = { "getSafeInsetLeft", "getSafeInsetTop", "getSafeInsetRight", "getSafeInsetBottom" };
        int* values[] = { &result.left, &result.top, &result.right, &result.bottom };
        for (int i = 0; i < 4; ++i) {
            jmethodID method = e->GetMethodID(cls, names[i], "()I");
            if (!method) break;
            *values[i] = e->CallIntMethod(cutout, method);
            if (e->ExceptionCheck()) break;
        }
        e->DeleteLocalRef(cls);
    }
    if (cutout) e->DeleteLocalRef(cutout);
    if (insets) e->DeleteLocalRef(insets);
    if (decor) e->DeleteLocalRef(decor);
    if (window) e->DeleteLocalRef(window);
    return result;
}

// Low resolution: the window buffer itself shrinks and the display hardware scales it up, so the
// frame never touches a native-size surface (render thread, once per change or new window).
void AndroidSetBufferSize(int width, int height) {
    auto* app = GetAndroidApp();
    if (!app || !app->window || (width == bufferW && height == bufferH)) return;
    if (ANativeWindow_setBuffersGeometry(app->window, width, height, 0) == 0) { bufferW = width; bufferH = height; }
}

void AndroidSetFrameRate(int fps) {
    if (android_get_device_api_level() < 30) return;
    auto* app = GetAndroidApp();
    if (!app || !app->window) return;
    using SetRate = int32_t (*)(ANativeWindow*, float, int8_t);
    static auto setRate = reinterpret_cast<SetRate>(dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
    if (setRate) setRate(app->window, static_cast<float>(std::max(0, fps)), 0);
}
}
