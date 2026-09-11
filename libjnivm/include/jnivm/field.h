#pragma once
#include "object.h"
#include <string>
#include <memory>

#include "methodhandlebase.h"

namespace jnivm {

    class Class;

    class Field : public Object {
    public:
        std::string name;
        std::string type;
        bool _static = false;
        // Set by ReflectionHelper / FakeJni registration so Unity's
        // Field.getDeclaringClass() can re-resolve instance fields.
        std::weak_ptr<Class> declaringClass;
        std::shared_ptr<MethodHandle> getnativehandle;
        std::shared_ptr<MethodHandle> setnativehandle;
#ifdef JNI_DEBUG
        std::string GenerateHeader();
        std::string GenerateStubs(std::string scope, const std::string &cname);
        std::string GenerateJNIBinding(std::string scope, const std::string &cname);
#endif
    };
}