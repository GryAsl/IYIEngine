// engine.play: Google Play bridge (Play Games Services v2, Saved Games, in-app review, LVL licence
// state). The Java helper (PlayServices.java) runs every async call and keeps the results in
// fields/maps; this file only reads them through the activity's JNI methods, so no Java thread ever
// calls into Python. Desktop and iOS compile the off values (SPEC §24.5).

#include "IYI/PlayBridge.h"

#ifdef IYI_PLATFORM_ANDROID

#include "AndroidJni.h"
#include <string>
#include <utility>

namespace IYI {
namespace {

// One JNI call into the activity: the thread, the activity and its class, released on scope exit.
struct Call {
    Jni jni;
    JNIEnv* e = nullptr;
    jobject activity = nullptr;
    jclass cls = nullptr;

    Call() {
        e = jni.env;
        auto* app = GetAndroidApp();
        if (!e || !app || !app->activity) return;
        activity = app->activity->clazz;
        cls = e->GetObjectClass(activity);
    }
    ~Call() { if (cls) e->DeleteLocalRef(cls); }
    jmethodID Method(const char* name, const char* signature) const {
        return activity && cls ? e->GetMethodID(cls, name, signature) : nullptr;
    }
    bool Ok() const { return e && activity && cls; }
};

jstring NewString(JNIEnv* e, const std::string& text) {
    return e->NewStringUTF(text.c_str());
}

} // namespace

namespace Api {

bool PlayAvailable() {
    Call c;
    jmethodID method = c.Method("playAvailable", "()Z");
    return method && c.e->CallBooleanMethod(c.activity, method) == JNI_TRUE;
}

void PlayUnlock(const std::string& achievementId) {
    Call c;
    jmethodID method = c.Method("playUnlock", "(Ljava/lang/String;)V");
    if (!method) return;
    jstring id = NewString(c.e, achievementId);
    c.e->CallVoidMethod(c.activity, method, id);
    if (id) c.e->DeleteLocalRef(id);
}

void PlayCloudWrite(const std::string& name, const std::string& data, int64_t modifiedMs) {
    Call c;
    jmethodID method = c.Method("playCloudWrite", "(Ljava/lang/String;[BJ)V");
    if (!method) return;
    jstring jname = NewString(c.e, name);
    jbyteArray bytes = c.e->NewByteArray(static_cast<jsize>(data.size()));
    if (bytes && !data.empty())
        c.e->SetByteArrayRegion(bytes, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte*>(data.data()));
    c.e->CallVoidMethod(c.activity, method, jname, bytes, static_cast<jlong>(modifiedMs));
    if (bytes) c.e->DeleteLocalRef(bytes);
    if (jname) c.e->DeleteLocalRef(jname);
}

void PlayCloudFetch(const std::string& name) {
    Call c;
    jmethodID method = c.Method("playCloudFetch", "(Ljava/lang/String;)V");
    if (!method) return;
    jstring jname = NewString(c.e, name);
    c.e->CallVoidMethod(c.activity, method, jname);
    if (jname) c.e->DeleteLocalRef(jname);
}

PlayCloud PlayCloudResult(const std::string& name) {
    PlayCloud result;
    Call c;
    jmethodID method = c.Method("playCloudResult", "(Ljava/lang/String;)[Ljava/lang/Object;");
    if (!method) return result;
    jstring jname = NewString(c.e, name);
    auto array = static_cast<jobjectArray>(c.e->CallObjectMethod(c.activity, method, jname));
    if (jname) c.e->DeleteLocalRef(jname);
    if (!array) return result;

    if (jstring status = static_cast<jstring>(c.e->GetObjectArrayElement(array, 0))) {
        const char* text = c.e->GetStringUTFChars(status, nullptr);
        if (text) {
            result.status = text;
            c.e->ReleaseStringUTFChars(status, text);
        }
        c.e->DeleteLocalRef(status);
    }
    if (jbyteArray bytes = static_cast<jbyteArray>(c.e->GetObjectArrayElement(array, 1))) {
        const jsize length = c.e->GetArrayLength(bytes);
        std::string data(static_cast<size_t>(length), '\0');
        if (length) c.e->GetByteArrayRegion(bytes, 0, length, reinterpret_cast<jbyte*>(&data[0]));
        result.data = std::move(data);
        c.e->DeleteLocalRef(bytes);
    }
    if (auto modified = static_cast<jlongArray>(c.e->GetObjectArrayElement(array, 2))) {
        jlong value = 0;
        c.e->GetLongArrayRegion(modified, 0, 1, &value);
        result.modifiedMs = value;
        c.e->DeleteLocalRef(modified);
    }
    c.e->DeleteLocalRef(array);
    return result;
}

std::optional<std::vector<std::string>> PlayCloudNames() {
    Call c;
    jmethodID method = c.Method("playCloudNames", "()[Ljava/lang/String;");
    auto array = method ? static_cast<jobjectArray>(c.e->CallObjectMethod(c.activity, method)) : nullptr;
    if (!array) return std::nullopt;
    std::vector<std::string> names;
    for (jsize i = 0, n = c.e->GetArrayLength(array); i < n; ++i) {
        auto name = static_cast<jstring>(c.e->GetObjectArrayElement(array, i));
        if (!name) continue;
        if (const char* utf = c.e->GetStringUTFChars(name, nullptr)) {
            names.emplace_back(utf);
            c.e->ReleaseStringUTFChars(name, utf);
        }
        c.e->DeleteLocalRef(name);
    }
    c.e->DeleteLocalRef(array);
    return names;
}

void PlayRequestReview() {
    Call c;
    jmethodID method = c.Method("playRequestReview", "()V");
    if (method) c.e->CallVoidMethod(c.activity, method);
}

std::string PlayLicenseState() {
    Call c;
    jmethodID method = c.Method("playLicenseState", "()Ljava/lang/String;");
    if (!method) return "off";
    auto text = static_cast<jstring>(c.e->CallObjectMethod(c.activity, method));
    if (!text) return "off";
    std::string state = "off";
    if (const char* utf = c.e->GetStringUTFChars(text, nullptr)) {
        state = utf;
        c.e->ReleaseStringUTFChars(text, utf);
    }
    c.e->DeleteLocalRef(text);
    return state;
}

} // namespace Api

} // namespace IYI

#else

// Desktop, iOS: nothing is configured, so every call is the off value.
namespace IYI {
namespace Api {

bool PlayAvailable() { return false; }
void PlayUnlock(const std::string&) {}
void PlayCloudWrite(const std::string&, const std::string&, int64_t) {}
void PlayCloudFetch(const std::string&) {}
PlayCloud PlayCloudResult(const std::string&) { return PlayCloud{}; }
std::optional<std::vector<std::string>> PlayCloudNames() { return std::nullopt; }
void PlayRequestReview() {}
std::string PlayLicenseState() { return "off"; }

} // namespace Api
} // namespace IYI

#endif
