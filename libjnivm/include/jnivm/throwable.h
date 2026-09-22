#pragma once
#include <jnivm/object.h>
#include <exception>
#include <stdexcept>

namespace jnivm {
    class Throwable : public Object {
    public:
        std::exception_ptr except;

        // Backing store for Throwable.setStackTrace()/getStackTrace().
        //
        // Held as a plain Object on purpose: the element type is
        // java/lang/StackTraceElement, which lives in the *loader's* javastubs
        // (javastubs/javac.h) and must not leak into libjnivm. The loader's
        // HookThrowableExtensions() fills and reads it, casting to the concrete
        // array type there. Null means "no stack trace recorded", which is what
        // a throwable that arrived over JNI Throw() legitimately looks like.
        std::shared_ptr<Object> stack_trace;
    };

    // Rethrow a modelled Java throwable as the C++ exception it wraps.
    //
    // `except` is only filled in by the paths that caught a real C++ exception
    // (a Method invocation's own catch(...), ThrowNew()). A throwable that
    // entered through JNI Throw(), or a default-constructed one handed back for
    // a missing member, arrives with an *empty* exception_ptr -- and
    // std::rethrow_exception() on that is fatal rather than merely wrong:
    // libstdc++ reads the __cxa_exception header 0x80 bytes below the exception
    // object, so an empty pointer becomes a load from 0xffffffffffffff80 and
    // kills the process (si_addr=-128, pc inside rethrow_exception()). Raise a
    // plain C++ exception instead, so a pending Java exception still unwinds the
    // way Java would.
    [[noreturn]] inline void RethrowThrowable(const Throwable* t) {
        if (t && t->except)
            std::rethrow_exception(t->except);
        throw std::runtime_error("pending Java exception without an attached C++ exception");
    }
}
