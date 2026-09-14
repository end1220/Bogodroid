#pragma once
#include <jni.h>
#include <memory>
#include <string>

namespace jnivm {
    class Class;
    class ENV;
    jclass InternalFindClass(JNIEnv *env, const char *name, bool returnZero = false, bool trace = false);
    std::shared_ptr<Class> InternalFindClass(ENV *env, const char *name, bool returnZero = false, bool trace = false);
    void Declare(JNIEnv *env, const char *signature);

    // Java binary names ("android.content.Context") and binary-flavoured JNI
    // signatures ("()Ljava.lang.Class;") show up wherever the guest resolved
    // types through Class.forName() — Unity 6 does that for every framework
    // class it touches and then keys all of its GetMethodID/GetFieldID calls
    // off the returned Class. The class registry and every descriptor here are
    // keyed by JNI internal names ("android/content/Context",
    // "()Ljava/lang/Class;"), so a dotted name used to auto-fabricate an empty
    // phantom class whose every lookup missed. JNI forbids '.' in class names
    // and signatures, so rewriting it is a no-op on canonical input and makes
    // the binary flavour resolve; '[' (arrays) and '$' (nested classes) are
    // left alone.
    inline std::string NormalizeDots(std::string name) {
        for (char& c : name) {
            if (c == '.')
                c = '/';
        }
        return name;
    }
}
