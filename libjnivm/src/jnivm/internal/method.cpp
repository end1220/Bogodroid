#include "method.h"
#include "log.h"
#include <jnivm/internal/jValuesfromValist.h>
#include <cstdio>
#include <mutex>
#include <unordered_set>
#include <string>
#include <cstring>

using namespace jnivm;

template<class T> static T dynamicValue(jvalue value) {
    T result{};
    static_assert(sizeof(T) <= sizeof(value), "JNI return value is too large");
    std::memcpy(&result, &value, sizeof(T));
    return result;
}

template<> void dynamicValue<void>(jvalue) {}

// Print each (kind, class, name, sig) tuple only once.
void jnivm::log_stub_miss_once(const char* kind, const char* cls, const char* meth, const char* sig) {
    static std::mutex mtx;
    static std::unordered_set<std::string> seen;
    std::string key = std::string(kind) + "|" + (cls?cls:"?") + "|" + (meth?meth:"?") + "|" + (sig?sig:"?");
    std::lock_guard<std::mutex> lock(mtx);
    if (seen.insert(key).second) {
        LOG("JNIVM", "[STUB-MISS] %s: Class=`%s` Member=`%s` Sig=`%s` -> returning default",
            kind, cls?cls:"???", meth?meth:"???", sig?sig:"???");
    }
}

// Rewrite every `L...;` class component of a signature to JNI slash form.
//
// Unity builds constructor signatures out of java.lang.Class.getName(), so a
// parameter class arrives dotted ("Lcom.unity3d.player.UnityPlayerActivity;")
// while every class in this tree registers itself in slash form. On a device
// that string is parsed by the Java half, which wants dots; jnivm uses it
// verbatim as a lookup key, so without this the two can never agree.
//
// Only L-components can contain dots, so one pass is enough; `$` (nested
// classes) and `[` (arrays) are passed through untouched.
static std::string normalize_jni_sig(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool inClass = false;
    for (char c : in) {
        if (c == 'L')
            inClass = true;
        else if (c == ';')
            inClass = false;

        out.push_back(inClass && c == '.' ? '/' : c);
    }
    return out;
}

template<bool isStatic, bool ReturnNull, bool AllowNative, bool trace>
jmethodID jnivm::GetMethodID(JNIEnv *env, jclass cl, const char *str0, const char *str1) {
    std::shared_ptr<Method> next;
    std::string sname = str0 ? str0 : "";
    std::string ssig = str1 ? str1 : "";
    auto cur = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), cl);
    if(str0 && std::strcmp(str0, "<init>") == 0) {
        LOG("BD-JNIVM", "[BD-CTOR-ENTER] cls=%s sig='%s' static=%d retNull=%d allowNative=%d trace=%d entries=%zu",
            cur ? cur->nativeprefix.c_str() : "(null)", str1 ? str1 : "(null)",
            (int)isStatic, (int)ReturnNull, (int)AllowNative, (int)trace,
            cur ? cur->methods.size() : 0);
    }
    if(cur) {
        // Rewrite init to Static external function
        //
        // The `!isStatic` guard that used to be here assumed a constructor is
        // only ever looked up through GetMethodID. Unity's IL2CPP
        // AndroidJNIHelper.GetConstructorID path reaches the static overload
        // instead, and with the guard in place that lookup skipped the rewrite,
        // compared the raw "(...)V" against constructors this tree registers in
        // the static "(...)L<class>;" form, missed, and degraded to an empty
        // stub. The caller then invoked that stub and got a default (null)
        // object back -- which is how com.mobge.assetlocator.AssetLocator
        // silently never existed.
        // Only rewrite the un-rewritten form. After the rewrite the return
        // slot holds L<class>; instead of V, which both marks the string as
        // already canonical and stops the recursion below from firing again.
        //
        // Both tests have to gate the rewrite *and* the else. An earlier
        // revision nested the void test inside the if body, so a lookup that
        // arrived already canonical (that same recursion) matched the name,
        // failed the inner test, and then fell past the search altogether --
        // reporting a miss against a table whose entry matched byte for byte.
        const auto close = ssig.find(')');
        const bool bdVoidCtor = close != std::string::npos &&
                                close + 1 < ssig.size() && ssig[close + 1] == 'V';
        if(sname == "<init>" && bdVoidCtor) {
            {
                std::lock_guard<std::mutex> lock(cur->mtx);
                ssig = normalize_jni_sig(ssig);
                ssig.erase(close + 1, std::string::npos);
                ssig.append("L");
                ssig.append(cur->nativeprefix);
                ssig.append(";");
            }
            return GetMethodID<true, ReturnNull, AllowNative, trace>(env, cl, str0, ssig.data());
        }
        else {
            std::lock_guard<std::mutex> lock(cur->mtx);
            std::string &classname = cur->name;
            auto ccl =
                    std::find_if(cur->methods.begin(), cur->methods.end(),
                                            [&sname, &ssig](std::shared_ptr<Method> &namesp) {
                                                return namesp->_static == isStatic && AllowNative == (bool)namesp->native && namesp->name == sname && namesp->signature == ssig;
                                            });
            if (ccl != cur->methods.end()) {
                next = *ccl;
            }
        }
    } else {
#ifdef JNI_DEBUG
        LOG("JNIVM", "Get%sMethodID class is null %s, %s", AllowNative ? "Native" : isStatic ? "Static" : "", str0, str1);
#endif
    }
    if(!next) {
        LOG("BD-JNIVM", "[BD-ANY-MISS] cls=%s str0='%s' str1='%s' static=%d allowNative=%d trace=%d entries=%zu",
            cur ? cur->nativeprefix.c_str() : "(null)", str0 ? str0 : "(null)",
            str1 ? str1 : "(null)", (int)isStatic, (int)AllowNative, (int)trace,
            cur ? cur->methods.size() : 0);
        // [BD] Report *every* miss, not only the ones that give up.
        //
        // Stock jnivm logs a miss only when ReturnNull makes it return null,
        // and only with NDEBUG off -- so the case that actually hurts a port
        // stayed invisible: a miss that silently materialises an empty stub
        // (nativehandle == nullptr) and hands it back as a valid jmethodID.
        // The caller invokes that stub, gets a default value, and carries on
        // holding a null object.
        if(trace) {
            log_stub_miss_once(
                AllowNative ? "Native MethodID" : isStatic ? "Static MethodID" : "MethodID",
                cur ? cur->nativeprefix.data() : "(null)",
                str0 ? str0 : "(null)",
                str1 ? str1 : "(null)");
        }
        // [BD] A miss on a class that already has entries, or on a constructor,
        // is the shape that matters: something is registered under that name and
        // the key still did not match it. Dump both sides.
        if(trace && cur) {
            std::lock_guard<std::mutex> lock(cur->mtx);
            if(!cur->methods.empty() || (str0 && std::strcmp(str0, "<init>") == 0)) {
                LOG("BD-JNIVM", "[BD-MISS] %s str0='%s' str1='%s' static=%d allowNative=%d entries=%zu",
                    cur->nativeprefix.c_str(), str0 ? str0 : "(null)", str1 ? str1 : "(null)",
                    (int)isStatic, (int)AllowNative, cur->methods.size());
                for (auto& m : cur->methods)
                    LOG("BD-JNIVM", "[BD-MISS]   name='%s' sig='%s' static=%d native=%p handle=%d",
                        m->name.c_str(), m->signature.c_str(), (int)m->_static, m->native,
                        (int)(bool)m->nativehandle);
            }
        }
        if(cur && cur->baseclasses) {
            for(auto&& i : cur->baseclasses(ENV::FromJNIEnv(env))) {
                if(i) {
                    auto id = GetMethodID<isStatic, true, AllowNative, false>(env, (jclass)i.get(), str0, str1);
                    if(id) {
                        return id;
                    }
                }
            }
        }
        if(ReturnNull) {
            return nullptr;
        }
        next = std::make_shared<Method>();
        if(cur) {
            cur->methods.push_back(next);
        } else {
            // For Debugging purposes without valid parent class
            JNITypes<std::shared_ptr<Method>>::ToJNIType(ENV::FromJNIEnv(env), next);
        }
        next->name = std::move(sname);
        next->signature = std::move(ssig);
        next->_static = isStatic;
#ifdef JNI_DEBUG
        Declare(env, next->signature.data());
#endif
#ifdef JNI_TRACE
        LOG("JNIVM", "Constructed Unresolved symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? "Native" : isStatic ? "Static" : "", str0, str1);
#endif
    } else {
#ifdef JNI_TRACE
        LOG("JNIVM", "Found symbol, Class=`%s`, %sMethod=`%s`, Signature=`%s`", cur ? cur->nativeprefix.data() : nullptr, AllowNative ? "Native" : isStatic ? "Static" : "", str0, str1);
#endif
    }
    return (jmethodID)next.get();
};

template<class T> T jnivm::defaultVal(ENV* env, std::string signature) {
    return {};
}
template<> void jnivm::defaultVal(ENV* env, std::string signature) {
}

// Consult VM::default_returns before falling back to defaultVal<T>.
// type_index match guards against "registered as int, read as bool".
template<class T> T jnivm::defaultValForMethod(ENV* env, const char* cls,
                                                const char* name,
                                                const std::string& signature) {
    if (cls && name && env) {
        auto& vm = *env->GetVM();
        std::string key;
        key.reserve(64);
        key.append(cls);
        key.push_back('|');
        key.append(name);
        key.push_back('|');
        key.append(signature);
        std::lock_guard<std::mutex> lock(vm.defaults_mtx);
        auto it = vm.default_returns.find(key);
        if (it != vm.default_returns.end() && it->second.type == typeid(T)) {
            T out{};
            it->second.writer(&out);
            return out;
        }
    }
    return defaultVal<T>(env, signature);
}
template<> void jnivm::defaultValForMethod(ENV* env, const char*, const char*,
                                            const std::string& signature) {
    defaultVal<void>(env, signature);
}

template<> jobject jnivm::defaultVal(ENV* env, std::string signature) {
    // Explicit factory registry — always active (each entry is opt-in).
    if(!signature.empty()) {
        size_t off = signature.find_last_of(")");
        if(signature[off + 1] == 'L' && signature[signature.size() - 1] == ';') {
            std::string cls_name = signature.substr(off + 2, signature.size() - (off + 3));
            auto& vm = *env->GetVM();
            std::function<std::shared_ptr<Object>()> ctor;
            {
                std::lock_guard<std::mutex> lock(vm.factories_mtx);
                auto it = vm.class_factories.find(cls_name);
                if (it != vm.class_factories.end()) {
                    ctor = it->second;
                }
            }
            if (ctor) {
                return JNITypes<std::shared_ptr<Object>>::ToJNIReturnType(env, ctor());
            }
        }
    }
#ifdef JNI_RETURN_NON_ZERO
    if(!signature.empty()) {
        size_t off = signature.find_last_of(")");
        if(signature[off + 1] == '[' ){
#ifdef JNI_TRACE
            LOG("JNIVM", "Construct array=`%s` via New*Array", &signature.data()[off + 1]);
#endif
            switch (signature[off + 2])
            {
            case 'B':
                return env->GetJNIEnv()->NewByteArray(0);
            case 'S':
                return env->GetJNIEnv()->NewShortArray(0);
            case 'C':
                return env->GetJNIEnv()->NewCharArray(0);
            case 'I':
                return env->GetJNIEnv()->NewIntArray(0);
            case 'J':
                return env->GetJNIEnv()->NewLongArray(0);
            case 'F':
                return env->GetJNIEnv()->NewFloatArray(0);
            case 'D':
                return env->GetJNIEnv()->NewDoubleArray(0);
            case 'L':
            case '[':
                return env->GetJNIEnv()->NewObjectArray(0, env->GetJNIEnv()->FindClass(signature[off + 2] == 'L' && signature[signature.size() - 1] == ';' ? signature.substr(off + 3, signature.size() - (off + 4)).data() : &signature.data()[off + 1]), nullptr);
            default:
#ifdef JNI_TRACE
            LOG("JNIVM", "Constructing array=`%s` failed unknown type", &signature.data()[off + 1]);
#endif
                break;
            }
            
        } else if(signature[off + 1] == 'L' && signature[signature.size() - 1] == ';'){
            auto c = env->GetClass(signature.substr(off + 2, signature.size() - (off + 3)).data());
            if(c->Instantiate) {
#ifdef JNI_TRACE
                LOG("JNIVM", "Construct object=`%s` via default constructor", &signature.data()[off + 1]);
#endif
                return JNITypes<std::shared_ptr<Object>>::ToJNIReturnType(env, c->Instantiate(env));
            } else {
                std::lock_guard<std::mutex> guard(env->GetVM()->mtx);
                bool safetocreatedummy = true;
                for(auto&& ty : env->GetVM()->typecheck) {
                    if(ty.second == c) {
                        safetocreatedummy = false;
                        break;
                    }
                }
                if(safetocreatedummy) {
#ifdef JNI_TRACE
                    LOG("JNIVM", "Construct dummy object=`%s`, no native type attached to this Class", &signature.data()[off + 1]);
#endif
                    auto dummy = std::make_shared<Object>();
                    dummy->clazz = c;
                    return JNITypes<std::shared_ptr<Object>>::ToJNIReturnType(env, dummy);
                } else {
#ifdef JNI_TRACE
                    LOG("JNIVM", "You have to create a default constructor for object=`%s` to get a non zero return value", &signature.data()[off + 1]);
#endif
                }
            }
        }
#ifdef JNI_TRACE
        LOG("JNIVM", "Failed to construct return value of signature=`%s`", signature.data());
#endif
    } else {
#ifdef JNI_TRACE
        LOG("JNIVM", "Failed to guess return value of empty signature");
#endif
    }
#endif
    return nullptr;
}

static Method* findNonVirtualOverload(Class*cl, Method*mid) {
    if(!cl || !mid) {
        return mid;
    }
    auto res = std::find_if(cl->methods.begin(), cl->methods.end(), [mid](auto&& m) {
        return !m->_static && mid->name == m->name && mid->signature == m->signature &&
               (m->nativehandle || m->dynamic);
    });
    if(res != cl->methods.end()) {
        return res->get();
    }
    return mid;
}

static Method* findVirtualOverload(jnivm::ENV *env, Class*cl, Method*mid) {
    if (!mid) return nullptr;
    if (cl) {
        auto res = std::find_if(cl->methods.begin(), cl->methods.end(), [mid](auto&& m) {
            return !m->_static && mid->name == m->name && mid->signature == m->signature &&
                   (m->nativehandle || m->dynamic);
        });
        if (res != cl->methods.end()) {
            return res->get();
        }
        if (cl->baseclasses) {
            for (auto&& base : cl->baseclasses(env)) {
                if (base) {
                    auto r2 = findVirtualOverload(env, base.get(), mid);
                    if (r2 && r2 != mid) return r2;
                }
            }
        }
    }
    return mid;
}

template<class T> T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, jvalue *param) {
    auto mid = ((Method *)id);
#ifdef JNI_DEBUG
    if(!obj)
        LOG("JNIVM", "CallMethod object is null");
    if(!id)
        LOG("JNIVM", "CallMethod field is null");
#endif
    if (mid && mid->dynamic) {
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(
            ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        mid = findVirtualOverload(ENV::FromJNIEnv(env), cl.get(), mid);
        if (mid && mid->dynamic)
            return dynamicValue<T>(mid->dynamic(env, obj, nullptr, param));
    }
    if (mid && mid->nativehandle) {
        auto orig_name = mid->name;
        auto orig_sig = mid->signature;
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        mid = findVirtualOverload(ENV::FromJNIEnv(env), cl.get(), mid);
        if (!mid || !mid->nativehandle) {
            log_stub_miss_once("Virtual",
                cl ? cl->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig.data());
            return defaultValForMethod<T>(ENV::FromJNIEnv(env),
                cl ? cl->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig);
        }
#ifdef JNI_TRACE
        LOG("JNIVM", "Call Member Function Class=`%s` Method=`%s` Signature=`%s`", cl ? cl->nativeprefix.data() : "???", mid->name.data(), mid->signature.data());
#endif
        try {
            return static_cast<jnivm::impl::MethodHandleBase<T>*>(mid->nativehandle.get())->InstanceInvoke(ENV::FromJNIEnv(env), obj, param, jnivm::impl::MethodHandleBase<T>{});
        } catch (...) {
            auto cur = std::make_shared<Throwable>();
            cur->except = std::current_exception();
            (ENV::FromJNIEnv(env))->current_exception = cur;
#ifdef JNI_TRACE
            env->ExceptionDescribe();
#endif
            return defaultValForMethod<T>(ENV::FromJNIEnv(env),
                cl ? cl->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig);
        }
    } else {
        auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), env->GetObjectClass(obj));
        log_stub_miss_once("Unknown Member",
            cl ? cl->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature.data() : nullptr);
        return defaultValForMethod<T>(ENV::FromJNIEnv(env),
            cl ? cl->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature : "");
    }
};

template<class T, class... Y> T jnivm::MDispatchBase<T, Y...>::CallMethod(JNIEnv *env, Y ...p, jmethodID id, va_list param) {
    if(id) {
        return MDispatch<T, Y...>::CallMethod(env, p..., id, JValuesfromValist(param, ((Method *)id)->signature.data()).data());
    } else {
#ifdef JNI_TRACE
        LOG("JNIVM", "CallMethod Method ID is null");
#endif
        return defaultVal<T>(ENV::FromJNIEnv(env), "");
    }
};

template<class T, class... Y> T jnivm::MDispatch<T, Y...>::CallMethod(JNIEnv *env, Y ...p, jmethodID id, ...) {
    va_list l;
    va_start(l, id);
    T ret = MDispatch<T, Y...>::CallMethod(env, p..., id, l);
    va_end(l);
    return ret;
};

template<class... Y> void jnivm::MDispatch<void, Y...>::CallMethod(JNIEnv *env, Y ...p, jmethodID id, ...) {
    va_list l;
    va_start(l, id);
    MDispatch<void, Y...>::CallMethod(env, p..., id, l);
    va_end(l);
};

template<class T> T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jclass cl, jmethodID id, jvalue *param) {
    auto mid = ((Method *)id);
#ifdef JNI_DEBUG
    if(!obj)
        LOG("JNIVM", "CallMethod object is null");
    if(!cl)
        LOG("JNIVM", "CallMethod class is null");
    if(!id)
        LOG("JNIVM", "CallMethod field is null");
#endif
    if (mid && mid->dynamic)
        return dynamicValue<T>(mid->dynamic(env, obj, cl, param));
    if (mid && mid->nativehandle) {
        auto orig_name = mid->name;
        auto orig_sig = mid->signature;
        auto clz = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), cl);
#ifdef JNI_TRACE
        LOG("JNIVM", "Call NonVirtual Member Function Class=`%s` Method=`%s` Signature=`%s`", clz ? clz->nativeprefix.data() : "???", mid->name.data(), mid ? mid->signature.data() : "???");
#endif
        mid = findNonVirtualOverload(clz.get(), mid);
        if (!mid || !mid->nativehandle) {
            log_stub_miss_once("NonVirtual",
                clz ? clz->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig.data());
            return defaultValForMethod<T>(ENV::FromJNIEnv(env),
                clz ? clz->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig);
        }
        try {
            return static_cast<jnivm::impl::MethodHandleBase<T>*>(mid->nativehandle.get())->NonVirtualInstanceInvoke(ENV::FromJNIEnv(env), obj, param, jnivm::impl::MethodHandleBase<T>{});
        } catch (...) {
            auto cur = std::make_shared<Throwable>();
            cur->except = std::current_exception();
            (ENV::FromJNIEnv(env))->current_exception = cur;
#ifdef JNI_TRACE
            env->ExceptionDescribe();
#endif
            return defaultValForMethod<T>(ENV::FromJNIEnv(env),
                clz ? clz->nativeprefix.data() : nullptr,
                orig_name.data(), orig_sig);
        }
    } else {
        auto clz = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), cl);
        log_stub_miss_once("Unknown NonVirtual",
            clz ? clz->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature.data() : nullptr);
        return defaultValForMethod<T>(ENV::FromJNIEnv(env),
            clz ? clz->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature : "");
    }
};

template<class T> T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jclass _cl, jmethodID id, jvalue *param) {
    auto cl = JNITypes<std::shared_ptr<Class>>::JNICast(ENV::FromJNIEnv(env), _cl);
    auto mid = ((Method *)id);
#ifdef JNI_DEBUG
    if(!cl)
        LOG("JNIVM", "CallMethod class is null");
    if(!id)
        LOG("JNIVM", "CallMethod method is null");
#endif
    if (mid && mid->dynamic)
        return dynamicValue<T>(mid->dynamic(env, nullptr, _cl, param));
    if (mid && mid->nativehandle) {
#ifdef JNI_TRACE
        LOG("JNIVM", "Call Static Function Class=`%s` Method=`%s` Signature=`%s`", cl ? cl->nativeprefix.data() : "???", mid->name.data(), mid ? mid->signature.data() : "???");
#endif
        try {
             return static_cast<jnivm::impl::MethodHandleBase<T>*>(mid->nativehandle.get())->StaticInvoke(ENV::FromJNIEnv(env), cl.get(), param, jnivm::impl::MethodHandleBase<T>{});
        } catch (...) {
            auto cur = std::make_shared<Throwable>();
            cur->except = std::current_exception();
            (ENV::FromJNIEnv(env))->current_exception = cur;
#ifdef JNI_TRACE
            env->ExceptionDescribe();
#endif
            return defaultVal<T>(ENV::FromJNIEnv(env), mid ? mid->signature : "");
        }
    } else {
        log_stub_miss_once("Unknown Static",
            cl ? cl->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature.data() : nullptr);
        return defaultValForMethod<T>(ENV::FromJNIEnv(env),
            cl ? cl->nativeprefix.data() : nullptr,
            mid ? mid->name.data() : nullptr,
            mid ? mid->signature : "");
    }
};

template jmethodID jnivm::GetMethodID<true>(JNIEnv *env, jclass cl, const char *str0, const char *str1);
template jmethodID jnivm::GetMethodID<false>(JNIEnv *env, jclass cl, const char *str0, const char *str1);
template jmethodID jnivm::GetMethodID<false, true, true>(JNIEnv *env, jclass cl, const char *str0, const char *str1);
template jmethodID jnivm::GetMethodID<false, true, false, false>(JNIEnv *env, jclass cl, const char *str0, const char *str1);
template jmethodID jnivm::GetMethodID<true, true, false, true>(JNIEnv *env, jclass cl, const char *str0, const char *str1);
template jmethodID jnivm::GetMethodID<false, true, false, true>(JNIEnv *env, jclass cl, const char *str0, const char *str1);

#define DeclareTemplate(T) template T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, jvalue *param);\
                           template T jnivm::MDispatchBase<T, jobject>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, va_list param);\
                           template T jnivm::MDispatch<T, jobject>::CallMethod(JNIEnv *env, jobject obj, jmethodID id, ...);\
                           template T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jclass obj, jmethodID id, jvalue *param);\
                           template T jnivm::MDispatchBase<T, jclass>::CallMethod(JNIEnv *env, jclass obj, jmethodID id, va_list param);\
                           template T jnivm::MDispatch<T, jclass>::CallMethod(JNIEnv *env, jclass obj, jmethodID id, ...);\
                           template T jnivm::MDispatchBase2<T>::CallMethod(JNIEnv *env, jobject obj, jclass c, jmethodID id, jvalue *param);\
                           template T jnivm::MDispatchBase<T, jobject, jclass>::CallMethod(JNIEnv *env, jobject obj, jclass c, jmethodID id, va_list param);\
                           template T jnivm::MDispatch<T, jobject, jclass>::CallMethod(JNIEnv *env, jobject obj, jclass c, jmethodID id, ...)
DeclareTemplate(jboolean);
DeclareTemplate(jbyte);
DeclareTemplate(jshort);
DeclareTemplate(jint);
DeclareTemplate(jlong);
DeclareTemplate(jfloat);
DeclareTemplate(jdouble);
DeclareTemplate(jchar);
DeclareTemplate(jobject);
DeclareTemplate(void);
#undef DeclareTemplate

#define DeclareTemplate(T) template T jnivm::defaultVal(ENV* env, std::string signature); \
                           template T jnivm::defaultValForMethod(ENV* env, const char* cls, const char* name, const std::string& sig)
DeclareTemplate(jboolean);
DeclareTemplate(jbyte);
DeclareTemplate(jshort);
DeclareTemplate(jint);
DeclareTemplate(jlong);
DeclareTemplate(jfloat);
DeclareTemplate(jdouble);
DeclareTemplate(jchar);
#undef DeclareTemplate
