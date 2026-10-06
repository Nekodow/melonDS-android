#include <jni.h>
#include <string>
#include "PMBridge.h"

namespace
{
std::string toStdString(JNIEnv* env, jstring str)
{
    if (!str) return "";
    const char* chars = env->GetStringUTFChars(str, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(str, chars);
    return result;
}

// Names come straight off the network: NewStringUTF aborts on invalid
// (modified) UTF-8, so keep valid 1-3 byte sequences and replace the rest.
std::string sanitize(const std::string& in)
{
    std::string out;
    size_t i = 0;
    while (i < in.size())
    {
        unsigned char c = (unsigned char)in[i];
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : 0;
        bool ok = len > 0 && i + len <= in.size() && !(len == 1 && c < 0x20 && c != '\n') && !(len == 2 && c < 0xC2);
        for (int k = 1; ok && k < len; k++)
            ok = ((unsigned char)in[i + k] & 0xC0) == 0x80;
        if (ok) { out.append(in, i, len); i += len; }
        else { out += '?'; i++; }
    }
    return out;
}
}

extern "C"
{

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_PMOnline_hostInternal(JNIEnv* env, jobject thiz, jstring relay, jstring name)
{
    return PMBridge::Host(toStdString(env, relay).c_str(), toStdString(env, name).c_str());
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_PMOnline_joinInternal(JNIEnv* env, jobject thiz, jstring relay, jstring code, jstring name)
{
    return PMBridge::Join(toStdString(env, relay).c_str(), toStdString(env, code).c_str(), toStdString(env, name).c_str());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_PMOnline_stop(JNIEnv* env, jobject thiz)
{
    PMBridge::Stop();
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_PMOnline_getDefaultRelayInternal(JNIEnv* env, jobject thiz)
{
    return env->NewStringUTF(PMBridge::DefaultRelay);
}

// Flat layout parsed by PMOnline.getStatus():
// [mode, peers, pending, code, server, text, myRole, name1, ping1, ..., name8, ping8, debug]
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_PMOnline_getStatusInternal(JNIEnv* env, jobject thiz)
{
    PMBridge::Status st = PMBridge::GetStatus();

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray result = env->NewObjectArray(7 + 8 * 2 + 1, stringClass, nullptr);

    int i = 0;
    auto put = [&](const std::string& value) {
        jstring s = env->NewStringUTF(sanitize(value).c_str());
        env->SetObjectArrayElement(result, i++, s);
        env->DeleteLocalRef(s);
    };

    put(std::to_string(st.mode));
    put(std::to_string(st.peers));
    put(st.pending ? "1" : "0");
    put(st.code);
    put(st.server);
    put(st.text);
    put(std::to_string(st.myRole));
    for (int r = 1; r <= 8; r++)
    {
        put(st.roster[r]);
        put(std::to_string(st.rosterPing[r]));
    }
    put(st.debug);
    return result;
}

}
