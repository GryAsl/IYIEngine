// Shared JNI glue for the Android shell (AndroidPlatform.cpp, AndroidPlayServices.cpp): attach the
// calling engine thread and call the activity's Java methods. Android only; included under
// IYI_PLATFORM_ANDROID.
#pragma once

#ifdef IYI_PLATFORM_ANDROID
#include <android_native_app_glue.h>
#include <jni.h>

extern "C" struct android_app* GetAndroidApp();

namespace IYI {

// Attaches the calling thread; detaches only when this call was the one that attached it.
struct Jni {
    JavaVM* vm = nullptr;
    JNIEnv* env = nullptr;
    bool attached = false;
    Jni() {
        auto* app = GetAndroidApp();
        if (!app || !app->activity) return;
        vm = app->activity->vm;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED)
            attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    }
    ~Jni() { if (env && env->ExceptionCheck()) env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); }
};

inline jobject CallObject(JNIEnv* e, jobject object, const char* name, const char* signature) {
    if (!object || e->ExceptionCheck()) return nullptr;
    jclass cls = e->GetObjectClass(object);
    jmethodID method = cls ? e->GetMethodID(cls, name, signature) : nullptr;
    jobject result = method ? e->CallObjectMethod(object, method) : nullptr;
    if (cls) e->DeleteLocalRef(cls);
    return result;
}

} // namespace IYI
#endif
