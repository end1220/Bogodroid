// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#include "javac.h"
#include "../globals.h"
#include "baron/baron.h"
#include "logging.h"
#include "toml++/toml.hpp"

#include <mutex>

extern toml::table config;

namespace {

// Unity does not hand ReflectionHelper.getConstructorID() a JNI signature: the
// managed side builds the string from java.lang.Class.getName(), so its class
// components come back dotted --
//
//     (Lcom.unity3d.player.UnityPlayerActivity;Ljava/lang/String;)V
//
// On a real device that is fine, because the Java half parses the string and
// feeds Class.forName(), which wants dots. jnivm does not parse anything: the
// string is used verbatim as a key into Class::methods, and every stub in this
// tree registers JNI (slash) form, so the lookup could never match. The miss
// silently degraded into a STUB-MISS `<init>` stub, Constructor.newInstance()
// returned null, and com.mobge.assetlocator.AssetLocator never came into
// existence -- which is what made MobGe.Storage.AndroidAssetManager throw
// NullReferenceException and the game present black frames.
//
// Rewrite every `L...;` class component to slash form. Only L-componenets can
// contain dots, so a single pass is enough; `$` (nested classes) and `[`
// (arrays) are left alone.
std::string bd_normalize_jni_sig(const std::string& in)
{
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

} // namespace

///// Long

jnivm::java::lang::reflect::Constructor::Constructor()
{
}

jnivm::java::lang::reflect::Constructor::Constructor(
    std::shared_ptr<jnivm::Class> clazz,
    std::shared_ptr<FakeJni::JString> constructorSignature)
    : targetClass(clazz)
{
    const char* sig = constructorSignature ? constructorSignature->c_str() : "()V";
    name = "<init>";
    signature = bd_normalize_jni_sig(sig);
    if (signature != sig)
        verbose("JavaReflect", "Constructor signature normalised '%s' -> '%s'",
                sig, signature.c_str());
}

std::shared_ptr<jnivm::Object>
jnivm::java::lang::reflect::Constructor::newInstance(std::shared_ptr<jnivm::Array<jnivm::Object>> args)
{
    if (!targetClass)
        return nullptr;

    FakeJni::LocalFrame frame(vm);
    auto env = &frame.getJniEnv();
    const jsize argCount = args ? args->getSize() : 0;
    std::vector<jvalue> values(argCount);
    for (jsize i = 0; i < argCount; ++i) {
        auto arg = args->Get(i, jnivm::impl::array_type_t<jnivm::Object>{});
        values[i].l = jnivm::JNITypes<std::shared_ptr<jnivm::Object>>::ToJNIType(jnivm::ENV::FromJNIEnv(env), arg);
    }

    auto cls = jnivm::JNITypes<std::shared_ptr<jnivm::Class>>::ToJNIType(jnivm::ENV::FromJNIEnv(env), targetClass);
    BD_LOG("JavaReflect", "newInstance(%s) ctor sig='%s' argc=%d",
           targetClass->getName().c_str(), signature.c_str(), (int)argCount);
    auto constructor = env->GetMethodID(cls, "<init>", signature.c_str());
    BD_LOG("JavaReflect", "  lookup -> %p", (void*)constructor);
    if (constructor) {
        auto* m = (jnivm::Method*)constructor;
        BD_LOG("JavaReflect", "  hit: name='%s' sig='%s' static=%d native=%p handle=%d",
               m->name.c_str(), m->signature.c_str(), (int)m->_static, m->native,
               (int)(bool)m->nativehandle);
    }
    if (!constructor) {
        BD_LOG("JavaReflect", "Constructor.newInstance could not resolve %s%s",
               targetClass->getName().c_str(), signature.c_str());
        // [BD] Temporary: a <init> lookup that fails against a class we *did*
        // register means the registration key and the lookup key disagree.
        // Print every entry so the two can be diffed directly instead of
        // reasoning about how Hook() renders the signature.
        auto c = jnivm::JNITypes<std::shared_ptr<jnivm::Class>>::JNICast(
            jnivm::ENV::FromJNIEnv(env), cls);
        if (c) {
            std::lock_guard<std::mutex> lock(c->mtx);
            BD_LOG("JavaReflect", "  lookup: name='<init>' sig='%s' prefix='%s' entries=%zu",
                   signature.c_str(), c->nativeprefix.c_str(), c->methods.size());
            for (auto& m : c->methods) {
                BD_LOG("JavaReflect", "  reg: name='%s' sig='%s' static=%d native=%p handle=%d",
                       m->name.c_str(), m->signature.c_str(), (int)m->_static, m->native,
                       (int)(bool)m->nativehandle);
            }
        }
        return nullptr;
    }
    auto obj = env->NewObjectA(cls, constructor, values.empty() ? nullptr : values.data());
    return jnivm::JNITypes<std::shared_ptr<jnivm::Object>>::JNICast(jnivm::ENV::FromJNIEnv(env), obj);
}

long jnivm::java::lang::Long::longValue()
{
    return this->value;
}

///// Boolean

// Initialize the static singleton instances
std::shared_ptr<jnivm::java::lang::Boolean> jnivm::java::lang::Boolean::TRUE_ = std::make_shared<Boolean>(JNI_TRUE);
std::shared_ptr<jnivm::java::lang::Boolean> jnivm::java::lang::Boolean::FALSE_ = std::make_shared<Boolean>(JNI_FALSE);

jnivm::java::lang::Boolean::Boolean(jboolean val)
    : value(val)
{
}

jboolean jnivm::java::lang::Boolean::booleanValue()
{
    return value;
}

// The valueOf factory is the standard way to get a Boolean.
// It's efficient because it reuses the static singleton objects.
std::shared_ptr<jnivm::java::lang::Boolean> jnivm::java::lang::Boolean::valueOf(jboolean val)
{
    return val ? TRUE_ : FALSE_;
}

///// Classloader

jnivm::java::lang::ClassLoader::ClassLoader(jnivm::Object* obj)
{
    verbose("JBRIDGE", "New Classloader for class %s", obj->getClass().getName().c_str());
    this->clazz = obj->clazz;
};

std::shared_ptr<FakeJni::JString> jnivm::java::lang::ClassLoader::findLibrary(std::shared_ptr<FakeJni::JString> name)
{
    verbose("JBRIDGE", "findLibrary stub %s", name.get()->c_str());
    return name;
}

///// StringBuilder

jnivm::java::lang::StringBuilder::StringBuilder()
{
    str = (FakeJni::JString) "";
}

std::shared_ptr<jnivm::java::lang::StringBuilder> jnivm::java::lang::StringBuilder::append(std::shared_ptr<FakeJni::JString> str_to_append)
{
    str = str.append(str_to_append.get()->c_str());
    return std::shared_ptr<jnivm::java::lang::StringBuilder>(this);
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::StringBuilder::toString()
{
    verbose("JBRIDGE", "toString: %s", str.c_str());
    return std::shared_ptr<FakeJni::JString>(&str);
}

///// InputStream

jnivm::java::io::InputStream::InputStream(std::shared_ptr<FakeJni::JString> filename)
{
    verbose("JBRIDGE", "IOStream opening file %s", filename.get()->c_str());
    std::ifstream* file = new std::ifstream(filename.get()->c_str(), std::ios::binary);
    if (!file->is_open()) {
        verbose("JBRIDGE", "IOStream ERROR on file %s", filename.get()->c_str());
    }
    this->file = file;
}

int jnivm::java::io::InputStream::read(
    std::shared_ptr<FakeJni::JByteArray> buffer,
    int offset,
    int length)
{
    if (!file || !file->is_open()) {
        // Java: If the first byte cannot be read for any reason other than EOF ⇒ throw IOException
        return -1;
    }

    if (!buffer) {
        // Java would throw NullPointerException; your framework may differ
        return -1;
    }

    if (offset < 0 || length < 0 || offset + length > buffer->getSize()) {
        // Java: IndexOutOfBoundsException
        return -1;
    }

    if (length == 0) {
        return 0;
    }

    char* dest = reinterpret_cast<char*>(buffer->getArray() + offset);

    // Read from current file position
    file->read(dest, length);
    std::streamsize count = file->gcount();

    if (count == 0) {
        // If at EOF before reading any bytes
        return -1;
    }

    return static_cast<int>(count);
}

///// File
static constexpr jlong BD_FILE_MOCK_FREE_BYTES = 32LL * 1024LL * 1024LL * 1024LL;
static constexpr jlong BD_FILE_MOCK_TOTAL_BYTES = 64LL * 1024LL * 1024LL * 1024LL;

jnivm::java::io::File::File(std::shared_ptr<FakeJni::JString> path)
{
    this->path = path;
}

static std::string bd_file_path_str(const std::shared_ptr<FakeJni::JString>& path)
{
    return path ? path->c_str() : std::string{};
}

static void bd_file_strip_trailing_sep(std::string& p)
{
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\'))
        p.pop_back();
}

std::shared_ptr<FakeJni::JString> jnivm::java::io::File::getPath()
{
    return path;
}

std::shared_ptr<FakeJni::JString> jnivm::java::io::File::getAbsolutePath()
{
    return getPath();
}

std::shared_ptr<FakeJni::JString> jnivm::java::io::File::getName()
{
    std::string p = bd_file_path_str(path);
    bd_file_strip_trailing_sep(p);
    if (p.empty())
        return std::make_shared<FakeJni::JString>("");
    const auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos)
        return std::make_shared<FakeJni::JString>(p);
    return std::make_shared<FakeJni::JString>(p.substr(pos + 1));
}

std::shared_ptr<FakeJni::JString> jnivm::java::io::File::getParent()
{
    std::string p = bd_file_path_str(path);
    bd_file_strip_trailing_sep(p);
    if (p.empty() || p == "/" || p == "\\")
        return nullptr;
    const auto pos = p.find_last_of("/\\");
    if (pos == std::string::npos)
        return nullptr;
    if (pos == 0)
        return std::make_shared<FakeJni::JString>("/");
    return std::make_shared<FakeJni::JString>(p.substr(0, pos));
}

std::shared_ptr<jnivm::java::io::File> jnivm::java::io::File::getParentFile()
{
    auto parent = getParent();
    if (!parent)
        return nullptr;
    return std::make_shared<File>(parent);
}

std::shared_ptr<FakeJni::JString> jnivm::java::io::File::toString()
{
    return getPath();
}

jlong jnivm::java::io::File::getFreeSpace()
{
    return BD_FILE_MOCK_FREE_BYTES;
}

jlong jnivm::java::io::File::getUsableSpace()
{
    return BD_FILE_MOCK_FREE_BYTES;
}

jlong jnivm::java::io::File::getTotalSpace()
{
    return BD_FILE_MOCK_TOTAL_BYTES;
}



///// Throwable subclasses

jnivm::java::lang::Error::Error(std::shared_ptr<FakeJni::JString> message)
    : message_(message)
{
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::Error::getMessage()
{
    return message_;
}

jnivm::java::lang::Exception::Exception(std::shared_ptr<FakeJni::JString> message)
    : message_(message)
{
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::Exception::getMessage()
{
    return message_;
}

///// java/lang/StackTraceElement

jnivm::java::lang::StackTraceElement::StackTraceElement(
    std::shared_ptr<FakeJni::JString> declaringClass,
    std::shared_ptr<FakeJni::JString> methodName,
    std::shared_ptr<FakeJni::JString> fileName,
    jint lineNumber)
    : declaringClass_(declaringClass)
    , methodName_(methodName)
    , fileName_(fileName)
    , lineNumber_(lineNumber)
{
    // Unity's exception path is the only caller, and it fails silently when this
    // yields a null element, so name every frame as it is built.
    BD_LOG("StackTrace", "%s.%s(%s:%d)",
           declaringClass_ ? declaringClass_->asStdString().c_str() : "?",
           methodName_ ? methodName_->asStdString().c_str() : "?",
           fileName_ ? fileName_->asStdString().c_str() : "Unknown Source",
           (int)lineNumber_);
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::StackTraceElement::getDeclaringClassName()
{
    return declaringClass_;
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::StackTraceElement::getMethodName()
{
    return methodName_;
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::StackTraceElement::getFileName()
{
    return fileName_;
}

jint jnivm::java::lang::StackTraceElement::getLineNumber()
{
    return lineNumber_;
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::StackTraceElement::toString()
{
    std::string s = declaringClass_ ? declaringClass_->asStdString() : "Unknown";
    s += ".";
    s += methodName_ ? methodName_->asStdString() : "?";
    if (fileName_) {
        s += "(";
        s += fileName_->asStdString();
        if (lineNumber_ >= 0)
            s += ":" + std::to_string(lineNumber_);
        s += ")";
    } else {
        s += "(Unknown Source)";
    }
    return std::make_shared<FakeJni::JString>(s);
}

///// Thread
#include <chrono>
#include <thread>
jnivm::java::lang::Thread::Thread(std::shared_ptr<FakeJni::JString> name) { }
jnivm::java::lang::Thread::Thread(std::shared_ptr<Runnable> runnable)
{
    // This constructor isn't strictly needed for HandlerThread but is good to have
}

void jnivm::java::lang::Thread::start()
{
    // The start method now creates a std::thread that executes our
    // private, JNI-safe entry point.
    native_thread = std::thread(&Thread::_thread_entry_point, this);
}

void jnivm::java::lang::Thread::_thread_entry_point()
{
    // This is the "wrapper" that runs on the new thread.
    JNIEnv* env = nullptr;
    int result = vm.AttachCurrentThread(&env, nullptr);
    if (result != JNI_OK || !env) {
        verbose("Thread", "Failed to attach thread '%s' to JNI VM.", name.c_str());
        return;
    }

    // Use a try/catch block to guarantee we always detach the thread,
    // even if the user's run() method throws an exception.
    try {
        // Now that the thread is safely attached, call the virtual run() method.
        // This will execute HandlerThread::run() or any other subclass's logic.
        this->run();
    } catch (const std::exception& e) {
        verbose("Thread", "Caught C++ exception in thread '%s': %s", name.c_str(), e.what());
        // You might want to forward this exception to a global handler here.
    } catch (...) {
        verbose("Thread", "Caught unknown exception in thread '%s'", name.c_str());
    }

    // The run() method has finished. Detach the thread before it exits.
    vm.DetachCurrentThread();
}

void jnivm::java::lang::Thread::run()
{
    // Default implementation does nothing. Subclasses (like HandlerThread)
    // will override this to do the actual work.
}

void jnivm::java::lang::Thread::join()
{
    if (native_thread.joinable()) {
        native_thread.join();
    }
}

///// System

long jnivm::java::lang::System::nanoTime()
{
    return time_point_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()).time_since_epoch().count();
}

jint jnivm::java::lang::System::identityHashCode(std::shared_ptr<jnivm::Object> object)
{
    // Java only promises stability for an object's lifetime, not a particular
    // hash algorithm. Fold the native object identity into a signed jint.
    const uintptr_t identity = reinterpret_cast<uintptr_t>(object.get());
    return static_cast<jint>(identity ^ (identity >> 32));
}

// System.load / loadLibrary: no-op. Games typically wrap these in a
// platform-detect path; on Bogodroid we have no Java-side classloader to
// register natives, so silent success is safer than throwing — throwing
// breaks games that don't have a UnsatisfiedLinkError catch on the call site.
void jnivm::java::lang::System::load(std::shared_ptr<FakeJni::JString> filename)
{
    BD_LOG("JNI", "System.load('%s') -> no-op (Bogodroid)",
            filename ? filename->asStdString().c_str() : "(null)");
}

void jnivm::java::lang::System::loadLibrary(std::shared_ptr<FakeJni::JString> libname)
{
    BD_LOG("JNI", "System.loadLibrary('%s') -> no-op (Bogodroid)",
            libname ? libname->asStdString().c_str() : "(null)");
}

///// FakeMethod

jnivm::java::lang::reflect::FakeMethod::FakeMethod(std::shared_ptr<FakeJni::JString> method)
{
    this->method_name = method;
}

std::shared_ptr<FakeJni::JString> jnivm::java::lang::reflect::FakeMethod::toString()
{
    verbose("JBRIDGE", "Reflect: Returning Method name %s", this->method_name.get()->c_str());
    return this->method_name;
}

///// Map

std::shared_ptr<jnivm::java::util::Set> jnivm::java::util::Map::entrySet()
{
    // Return an empty set (so Unity just sees no prefs).
    return std::make_shared<Set>();
}

///// Set

std::shared_ptr<jnivm::java::util::Iterator> jnivm::java::util::Set::iterator()
{
    return std::make_shared<Iterator>();
}

///// List

std::shared_ptr<jnivm::java::util::Iterator> jnivm::java::util::List::iterator()
{
    // Return a new instance of your existing empty iterator. This is perfect.
    return std::make_shared<Iterator>();
}

int jnivm::java::util::List::size()
{
    return 0; // The list is always empty for now.
}

bool jnivm::java::util::List::isEmpty()
{
    return size() == 0;
}

///// ArrayList

// ArrayList Implementation
std::shared_ptr<jnivm::java::util::Iterator> jnivm::java::util::ArrayList::iterator()
{
    auto self = std::dynamic_pointer_cast<ArrayList>(shared_from_this());
    if (!self) {
        return nullptr;
    }
    return std::make_shared<ArrayListIterator>(std::move(self));
}

int jnivm::java::util::ArrayList::size() { return elements.size(); }

bool jnivm::java::util::ArrayList::isEmpty() { return elements.empty(); }
void jnivm::java::util::ArrayList::add(std::shared_ptr<FakeJni::JObject> obj) { elements.push_back(obj); }
std::shared_ptr<FakeJni::JObject> jnivm::java::util::ArrayList::get(int index) { return elements.at(index); }

///// ArrayListIterator

// ArrayListIterator Implementation
jnivm::java::util::ArrayListIterator::ArrayListIterator(std::shared_ptr<jnivm::java::util::ArrayList> l)
    : list(l)
    , index(0)
{
}

bool jnivm::java::util::ArrayListIterator::hasNext()
{
    return index < list.get()->size();
}

std::shared_ptr<FakeJni::JObject> jnivm::java::util::ArrayListIterator::next()
{
    return list.get()->get(index++);
}

///// Locale

// Read [locale] tag = "zh-CN" from toml; default en-US.
// Games like Hollow Knight pick UI language from
// java.util.Locale.getDefault().getLanguage().
static std::string bd_locale_tag()
{
    return config["locale"]["tag"].value_or<std::string>("en-US");
}
static std::string bd_locale_language()
{
    auto tag = bd_locale_tag();
    auto dash = tag.find_first_of("-_");
    return dash == std::string::npos ? tag : tag.substr(0, dash);
}
static std::string bd_locale_country()
{
    auto tag = bd_locale_tag();
    auto dash = tag.find_first_of("-_");
    return dash == std::string::npos ? std::string{} : tag.substr(dash + 1);
}

std::shared_ptr<jnivm::java::util::Locale> jnivm::java::util::Locale::getDefault()
{
    return std::make_shared<jnivm::java::util::Locale>();
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Locale::toLanguageTag()
{
    return std::make_shared<FakeJni::JString>(bd_locale_tag());
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Locale::getLanguage()
{
    return std::make_shared<FakeJni::JString>(bd_locale_language());
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Locale::getCountry()
{
    return std::make_shared<FakeJni::JString>(bd_locale_country());
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Locale::toString()
{
    auto tag = bd_locale_tag();
    // toString uses underscore: zh_CN
    for (auto& c : tag) if (c == '-') c = '_';
    return std::make_shared<FakeJni::JString>(tag);
}

///// Integer (java.lang.Integer wrapper)

jint jnivm::java::lang::Integer::intValue()    { return value; }
jlong jnivm::java::lang::Integer::longValue()  { return (jlong)value; }
jfloat jnivm::java::lang::Integer::floatValue(){ return (jfloat)value; }
jdouble jnivm::java::lang::Integer::doubleValue(){ return (jdouble)value; }

std::shared_ptr<FakeJni::JString> jnivm::java::lang::Integer::toString()
{
    return std::make_shared<FakeJni::JString>(std::to_string(value).c_str());
}

std::shared_ptr<jnivm::java::lang::Integer> jnivm::java::lang::Integer::valueOf(jint v)
{
    return std::make_shared<jnivm::java::lang::Integer>(v);
}

jint jnivm::java::lang::Integer::parseInt(std::shared_ptr<FakeJni::JString> s)
{
    if (!s) return 0;
    try { return std::stoi(s->asStdString()); }
    catch (...) { return 0; }
}

///// StringStubs (factory for empty String — see javac.h for rationale)
std::shared_ptr<FakeJni::JString> jnivm::java::lang::StringStubs::initEmpty()
{
    return std::make_shared<FakeJni::JString>();
}

///// Iterator

bool jnivm::java::util::Iterator::hasNext()
{
    return false; // Always empty
}

std::shared_ptr<FakeJni::JObject> jnivm::java::util::Iterator::next()
{
    return nullptr; // Never called if hasNext = false
}
///// Scanner

jnivm::java::util::Scanner::Scanner(std::shared_ptr<jnivm::java::io::InputStream> stream, std::shared_ptr<FakeJni::JString> str)
{
    verbose("JBRIDGE", "initialized Scanner with stream %p and str %s", stream.get(), str.get()->c_str());
    istream = stream;
}

std::shared_ptr<jnivm::java::util::Scanner> jnivm::java::util::Scanner::useDelimiter(std::shared_ptr<FakeJni::JString> str)
{
    verbose("JBRIDGE", "Scanner using delimiter %s", str.get()->c_str());
    delimiter = *str;
    return std::shared_ptr<Scanner>(this);
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Scanner::next()
{
    std::string str;
    (istream->file)->seekg(0, std::ios::end);
    str.reserve((istream->file)->tellg());
    (istream->file)->seekg(0, std::ios::beg);

    str.assign((std::istreambuf_iterator<char>(*(istream->file))),
        std::istreambuf_iterator<char>());
    verbose("JBRIDGE", "Scanner returning: %s", str.c_str());
    return std::make_shared<FakeJni::JString>(str);
}

std::shared_ptr<FakeJni::JString> jnivm::java::util::Scanner::nextLine()
{
    std::string str;
    (istream->file)->seekg(0, std::ios::end);
    str.reserve((istream->file)->tellg());
    (istream->file)->seekg(0, std::ios::beg);

    str.assign((std::istreambuf_iterator<char>(*(istream->file))),
        std::istreambuf_iterator<char>());
    verbose("JBRIDGE", "Scanner returning: %s", str.c_str());
    return std::make_shared<FakeJni::JString>(str);
}

// Descriptors

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::reflect::Constructor) { FakeJni::Constructor<Constructor> {} },
    { FakeJni::Constructor<Constructor, std::shared_ptr<jnivm::Class>, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&Constructor::newInstance> {}, "newInstance", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Long) { FakeJni::Constructor<Long, jlong> {} },
    { FakeJni::Function<&Long::longValue> {}, "longValue", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    // Registered so defaultVal()/STUB-MISS can build a real Throwable-derived
    // object instead of a dummy Object (see javac.h).
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Error) { FakeJni::Constructor<Error> {} },
    { FakeJni::Constructor<Error, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&Error::getMessage> {}, "getMessage", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Exception) { FakeJni::Constructor<Exception> {} },
    { FakeJni::Constructor<Exception, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&Exception::getMessage> {}, "getMessage", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    // Unity builds one per frame while reporting a managed exception. jnivm
    // rewrites the "<init>(...)V" Unity asks for into the static
    // "(...)Ljava/lang/StackTraceElement;" it actually looks up, which is what
    // FakeJni::Constructor here produces.
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::StackTraceElement)
    { FakeJni::Constructor<StackTraceElement, std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>, jint> {} },
    { FakeJni::Function<&StackTraceElement::getDeclaringClassName> {}, "getClassName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&StackTraceElement::getMethodName> {}, "getMethodName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&StackTraceElement::getFileName> {}, "getFileName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&StackTraceElement::getLineNumber> {}, "getLineNumber", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&StackTraceElement::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Boolean) { FakeJni::Constructor<Boolean, jboolean> {} },
    { FakeJni::Function<&Boolean::booleanValue> {}, "booleanValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Boolean::valueOf> {}, "valueOf", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::ClassLoader) { FakeJni::Constructor<ClassLoader, Object*> {} },
    { FakeJni::Function<&ClassLoader::findLibrary> {}, "findLibrary", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::StringBuilder) { FakeJni::Constructor<StringBuilder> {} },
    { FakeJni::Function<&StringBuilder::append> {}, "append", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&StringBuilder::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::io::InputStream) { FakeJni::Constructor<InputStream, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&InputStream::read> {}, "read", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::io::File) { FakeJni::Constructor<File, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&File::getPath> {}, "getPath", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getAbsolutePath> {}, "getAbsolutePath", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getName> {}, "getName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getParent> {}, "getParent", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getParentFile> {}, "getParentFile", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getFreeSpace> {}, "getFreeSpace", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getUsableSpace> {}, "getUsableSpace", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&File::getTotalSpace> {}, "getTotalSpace", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::io::FileDescriptor) { FakeJni::Constructor<FileDescriptor> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::Map) { FakeJni::Constructor<Map> {} },
    { FakeJni::Function<&Map::entrySet> {}, "entrySet", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR
    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::Set) { FakeJni::Constructor<Set> {} },
    { FakeJni::Function<&Set::iterator> {}, "iterator", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::List) { FakeJni::Constructor<List> {} },
    { FakeJni::Function<&List::iterator> {}, "iterator", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&List::size> {}, "size", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&List::isEmpty> {}, "isEmpty", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::ArrayList) { FakeJni::Constructor<ArrayList> {} },
    { FakeJni::Function<&ArrayList::iterator> {}, "iterator", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&ArrayList::size> {}, "size", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&ArrayList::isEmpty> {}, "isEmpty", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&ArrayList::add> {}, "add", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::Iterator) { FakeJni::Constructor<Iterator> {} },
    { FakeJni::Function<&Iterator::hasNext> {}, "hasNext", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Iterator::next> {}, "next", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::ArrayListIterator) { FakeJni::Function<&ArrayListIterator::hasNext> {}, "hasNext", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&ArrayListIterator::next> {}, "next", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::Locale) { FakeJni::Constructor<Locale> {} },
    { FakeJni::Function<&Locale::getDefault> {}, "getDefault", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Locale::toLanguageTag> {}, "toLanguageTag", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Locale::getLanguage> {}, "getLanguage", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Locale::getCountry> {}, "getCountry", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Locale::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::StringStubs)
    { FakeJni::Function<&StringStubs::initEmpty> {}, "<init>", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Integer)
    { FakeJni::Constructor<Integer> {} },
    { FakeJni::Constructor<Integer, jint> {} },
    { FakeJni::Function<&Integer::intValue> {}, "intValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Integer::longValue> {}, "longValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Integer::floatValue> {}, "floatValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Integer::doubleValue> {}, "doubleValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Integer::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Integer::valueOf> {}, "valueOf", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Integer::parseInt> {}, "parseInt", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Runnable) { FakeJni::Function<&Runnable::run> {}, "run", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::LambdaRunnable)
        END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::System) { FakeJni::Constructor<System> {} },
    { FakeJni::Function<&System::nanoTime> {}, "nanoTime", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&System::identityHashCode> {}, "identityHashCode", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&System::load> {}, "load", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&System::loadLibrary> {}, "loadLibrary", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::Thread) { FakeJni::Constructor<Thread, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Constructor<Thread, std::shared_ptr<jnivm::java::lang::Runnable>> {} },
    { FakeJni::Function<&Thread::start> {}, "start", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Thread::join> {}, "join", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::lang::reflect::FakeMethod) { FakeJni::Constructor<FakeMethod, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&FakeMethod::toString> {}, "toString", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::java::util::Scanner) { FakeJni::Constructor<Scanner, std::shared_ptr<jnivm::java::io::InputStream>, std::shared_ptr<FakeJni::JString>> {} },
    { FakeJni::Function<&Scanner::useDelimiter> {}, "useDelimiter", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Scanner::next> {}, "next", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Scanner::next> {}, "nextLine", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    void InitJNIJavaClasses(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Initializing Java JNI Classes");
    vm->registerClass<jnivm::java::lang::reflect::Constructor>();
    vm->registerClass<jnivm::java::lang::Long>();
    vm->registerClass<jnivm::java::lang::Error>();
    vm->registerClass<jnivm::java::lang::Exception>();
    vm->registerClass<jnivm::java::lang::StackTraceElement>();
    vm->registerClass<jnivm::java::lang::Boolean>();
    vm->registerClass<jnivm::java::lang::ClassLoader>();
    vm->registerClass<jnivm::java::lang::StringBuilder>();
    vm->registerClass<jnivm::java::lang::Thread>();
    vm->registerClass<jnivm::java::lang::Runnable>();
    vm->registerClass<jnivm::java::lang::LambdaRunnable>(); // Non-standard, for internal use
    vm->registerClass<jnivm::java::lang::System>();
    vm->registerClass<jnivm::java::lang::reflect::FakeMethod>();
    vm->registerClass<jnivm::java::io::InputStream>();
    vm->registerClass<jnivm::java::io::File>();
    vm->registerClass<jnivm::java::io::FileDescriptor>();
    vm->registerClass<jnivm::java::util::Map>();
    vm->registerClass<jnivm::java::util::Set>();
    vm->registerClass<jnivm::java::util::List>();
    vm->registerClass<jnivm::java::util::ArrayList>();
    vm->registerClass<jnivm::java::util::Iterator>();
    vm->registerClass<jnivm::java::util::ArrayListIterator>();
    vm->registerClass<jnivm::java::util::Scanner>();
    vm->registerClass<jnivm::java::util::Locale>();
    vm->registerClass<jnivm::java::lang::Integer>();
    vm->registerClass<jnivm::java::lang::StringStubs>();
}

///// Extensions to built-in Java classes

void HookStringExtensions(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Hooking String Extensions");
    FakeJni::LocalFrame frame(*vm);
    auto stringClass = vm->findClass("java/lang/String");

    // String.equals
    stringClass->HookInstanceFunction(&frame.getJniEnv(), "equals", [](jnivm::ENV* env, jnivm::Object* self, jnivm::Object* obj) {
        verbose("JBRIDGE", "String %s == %s = %d", (*dynamic_cast<FakeJni::JString*>(self)).c_str(), (*dynamic_cast<FakeJni::JString*>(obj)).c_str(), (*dynamic_cast<FakeJni::JString*>(self)) == (*dynamic_cast<FakeJni::JString*>(obj)));
        return (*dynamic_cast<FakeJni::JString*>(self)) == (*dynamic_cast<FakeJni::JString*>(obj));
    });

    // Hook constructor with lambda
    stringClass->Hook(&frame.getJniEnv(), "<init>",
        [](jnivm::ENV* env, jnivm::Class* c, std::shared_ptr<jnivm::Array<jbyte>> bytes,
            std::shared_ptr<jnivm::String> charset) -> std::shared_ptr<jnivm::String> {
            std::string charsetName = charset.get()->asStdString();
            auto byteData = bytes.get()->getArray();
            auto byteSize = bytes.get()->getSize();
            std::string result = "";

            if (charsetName == "UTF-8") {
                result = std::string(reinterpret_cast<const char*>(byteData),
                    byteSize);
            }
            return std::make_shared<jnivm::String>(result);
        });

    // String.getBytes with specified charset
    stringClass->HookInstanceFunction(&frame.getJniEnv(), "getBytes",
        [](jnivm::ENV* env, jnivm::Object* self, std::shared_ptr<jnivm::String> charset)
            -> std::shared_ptr<jnivm::Array<jbyte>> {
            std::string charsetName = charset.get()->asStdString();
            std::string stringValue = (*dynamic_cast<FakeJni::JString*>(self)).asStdString();

            std::vector<jbyte> bytes;

            if (charsetName == "UTF-8") {
                bytes.assign(stringValue.begin(), stringValue.end());
            }

            auto result = std::make_shared<FakeJni::JByteArray>(bytes);
            return result;
        });
}

// void HookReflectExtensions(FakeJni::Jvm* vm)
// {
//     verbose("JBRIDGE", "Hooking Reflect Extensions");
//     FakeJni::LocalFrame frame(*vm);
//     auto methodClass = vm->findClass("java/lang/reflect/Method");

//     methodClass->HookInstanceFunction(&frame.getJniEnv(), "toString", [](jnivm::ENV* env, jnivm::Object* self, jnivm::Object* obj) {
//         verbose("JBRIDGE", "ToString on Reflect Method called");
//         return NULL;
//     });

// }

void HookIntExtensions(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Hooking Int Extensions");
    FakeJni::LocalFrame frame(*vm);
    auto intClass = vm->findClass("java/lang/Integer");

    intClass->Hook(&frame.getJniEnv(), "parseInt", [vm](std::shared_ptr<FakeJni::JString> string) {
        verbose("JBRIDGE", "String Convert: %s", string.get()->c_str());
        return std::stoi(string.get()->asStdString());
    });
}

void HookClassExtensions(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Hooking Class Extensions");
    FakeJni::LocalFrame frame(*vm);
    auto classClass = vm->findClass("java/lang/Class");

    // Class.getClassLoader
    classClass->HookInstanceFunction(&frame.getJniEnv(), "getClassLoader", [](jnivm::ENV* env, jnivm::Object* self) {
        verbose("JBRIDGE", "getClassLoader for Class %s", self->getClass().getName().c_str());
        return std::make_shared<jnivm::java::lang::ClassLoader>(self);
    });

    // Class.forName (static)
    //
    // Registered with ONE parameter on purpose, even though the JVM also has
    // forName(String, boolean, ClassLoader) and ReflectionHelper::getMethodID's
    // name-only search will happily hand this method out for a lookup done with
    // the 3-arg signature.
    //
    // jnivm's varargs dispatch builds the argument list from the *lambda's*
    // arity, so a 3-arg hook reads values[1] and values[2] unconditionally. When
    // the caller only built a 1-element jvalue array -- Unity's
    // AndroidJavaClass path does exactly that -- those two reads land in
    // untouched stack slots, and the ClassLoader unpack dynamic_casts a stack
    // address. Under-reading, by contrast, is harmless: extra jvalues the hook
    // never looks at are simply ignored. forName(name) is also semantically
    // identical to forName(name, true, null), which is what our ClassLoader stub
    // would resolve to anyway.
    //
    // The name also has to be translated: java.lang.Class.forName() takes a
    // *binary* name ("com.unity3d.player.UnityPlayer") while jnivm's class
    // registry is keyed by the JNI name it was registered under
    // ("com/unity3d/player/UnityPlayer", findclass.cpp assigns nativeprefix =
    // the name findClass was called with). Passing the dotted form straight
    // through used to miss the registered class and silently mint a fresh,
    // empty auto-stub in its place. Everything downstream then read that empty
    // class: Unity's getFieldID("currentActivity") found no such field, so the
    // managed side saw currentActivity == null and MobGe's Android platform
    // layer threw NullReferenceException out of
    // AndroidAssetLocator..cctor / PlatformHolderNativeAndroid..ctor before a
    // single frame was ever presented.
    classClass->Hook(&frame.getJniEnv(), "forName", [vm](std::shared_ptr<FakeJni::JString> name) {
        if (name == nullptr) {
            verbose("JBRIDGE", "Class forName (null name)");
            return std::shared_ptr<jnivm::Class>(nullptr);
        }
        std::string binaryName = name->asStdString();
        verbose("JBRIDGE", "Class forName %s", binaryName.c_str());
        std::string jniName = binaryName;
        std::replace(jniName.begin(), jniName.end(), '.', '/');
        return vm->findClass(jniName.c_str());
    });

    // Class.getName
    classClass->HookInstanceFunction(&frame.getJniEnv(), "getName", [](jnivm::ENV* env, jnivm::Object* self) -> std::shared_ptr<FakeJni::JString> {
        auto representedClass = dynamic_cast<jnivm::Class*>(self);
        std::string name = representedClass
            ? representedClass->getName()
            : self->getClass().getName();
        // java.lang.Class.getName() uses binary Java names, while jnivm stores
        // JNI internal names. Unity's proxy unboxer compares against names such
        // as "java.lang.Integer", so slash-separated names prevent primitive
        // wrappers from being unboxed and make proxy method lookup fail.
        std::replace(name.begin(), name.end(), '/', '.');
        verbose("JBRIDGE", "getName for Class %s", name.c_str());
        return std::make_shared<FakeJni::JString>(name);
    });

    // AndroidJNIHelper inspects every boxed proxy argument with Class.isArray.
    // Class objects in jnivm represent array types using their JNI descriptor.
    classClass->HookInstanceFunction(&frame.getJniEnv(), "isArray", [](jnivm::ENV*, jnivm::Object* self) -> bool {
        auto representedClass = dynamic_cast<jnivm::Class*>(self);
        const bool result = representedClass && !representedClass->getName().empty() &&
                            representedClass->getName().front() == '[';
        verbose("JBRIDGE", "isArray for Class %s -> %d",
                representedClass ? representedClass->getName().c_str() : "(invalid)", result);
        return result;
    });
}

void HookObjectExtensions(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Hooking Object Extensions");
    FakeJni::LocalFrame frame(*vm);
    auto objClass = vm->findClass("java/lang/Object");

    // Object.getClass
    objClass->HookInstanceFunction(&frame.getJniEnv(), "getClass", [](jnivm::ENV* env, jnivm::Object* self) {
        // jnivm::String (FakeJni::JString) is a built-in value type and does
        // not carry a normal Class descriptor.  When it is boxed into the
        // Object[] used by Unity's proxy bridge, Java code must still see the
        // runtime type java.lang.String; reporting java.lang.Object makes
        // AndroidJNIHelper reject the argument and raises a spurious
        // NullReferenceException while processing PAD callbacks.
        if (dynamic_cast<FakeJni::JString*>(self)) {
            verbose("JBRIDGE", "getClass for String -> java/lang/String");
            return env->GetClass("java/lang/String");
        }
        verbose("JBRIDGE", "getClass for Object %s", self->getClass().getName().c_str());
        return env->GetClass(self->getClass().getName().c_str());
    });

    objClass->HookInstanceFunction(&frame.getJniEnv(), "toString", [](jnivm::ENV* env, jnivm::Object* self) {
        if (auto string = dynamic_cast<FakeJni::JString*>(self)) {
            // java.lang.String.toString() returns the string itself.  Unity's
            // managed proxy unboxer calls this through Object after checking
            // the runtime class, so a placeholder here corrupts PAD pack names.
            return std::make_shared<FakeJni::JString>(string->asStdString());
        }
        verbose("JBRIDGE", "toString for Object %s", self->getClass().getName().c_str());
        return std::make_shared<FakeJni::JString>("I dunno");
    });
}

///// Throwable extensions

// jnivm::Throwable is on the codegen blacklist (internal/codegen/class.cpp), so
// it has no DEFINE_CLASS_NAME and cannot own a BEGIN_NATIVE_DESCRIPTOR block.
// Its extra methods therefore have to be attached to the live Class objects,
// exactly the way HookClassExtensions() attaches Class.getName.
//
// Why it matters: Unity's managed-exception path calls
// Throwable.setStackTrace([Ljava/lang/StackTraceElement;)V on the java/lang/Error
// it has just built. jnivm first looks on java/lang/Error, then walks its
// baseclasses, finds nothing on java/lang/Throwable either, and answers with the
// STUB-MISS default -- so the exception reaches Unity's reporter with a null
// stack trace. Hooking every plausible throwable name means the lookup succeeds
// whichever class the object happens to report.
//
// getStackTrace() is the read side of the same storage; it is registered even
// though Oddmar has not been seen calling it, because a missing getter turns a
// "null stack trace" into a "method not found" on any Unity version that does.
namespace {
    // Class::HookInstanceFunction() deduces its signature from the callable's
    // operator(). Passing a lambda *variable* would bind it as an lvalue
    // reference, which jnivm::Function cannot introspect ("'operator()' is not a
    // member of ...&"), so hand the hooks over as prvalues from these factories.
    // The hook is stored by value per class, hence one factory call per class.
    auto makeSetStackTraceHook()
    {
        return [](jnivm::ENV* env, jnivm::Object* self,
                std::shared_ptr<jnivm::Array<jnivm::java::lang::StackTraceElement>> trace) {
            auto throwable = dynamic_cast<jnivm::Throwable*>(self);
            const jsize frames = trace ? trace->getSize() : 0;
            if (throwable)
                throwable->stack_trace = trace;
            BD_LOG("Throwable", "setStackTrace(%d frame%s)%s", (int)frames,
                   frames == 1 ? "" : "s",
                   throwable ? "" : " on a non-throwable receiver");
        };
    }

    auto makeGetStackTraceHook()
    {
        return [](jnivm::ENV* env, jnivm::Object* self)
                -> std::shared_ptr<jnivm::Array<jnivm::java::lang::StackTraceElement>> {
            auto throwable = dynamic_cast<jnivm::Throwable*>(self);
            if (!throwable || !throwable->stack_trace)
                return nullptr;
            return std::dynamic_pointer_cast<jnivm::Array<jnivm::java::lang::StackTraceElement>>(
                throwable->stack_trace);
        };
    }
}

void HookThrowableExtensions(FakeJni::Jvm* vm)
{
    verbose("JBRIDGE", "Hooking Throwable Extensions");
    FakeJni::LocalFrame frame(*vm);

    // Unity names the throwable after the class it constructed; covering the
    // common java.lang ones as well means a RuntimeException from game code gets
    // the same treatment as the Error we have actually seen.
    static const char* const throwableClasses[] = {
        "java/lang/Throwable",
        "java/lang/Error",
        "java/lang/Exception",
        "java/lang/RuntimeException",
        "java/lang/NullPointerException",
        "java/lang/IllegalStateException",
        "java/lang/IllegalArgumentException",
        "java/lang/UnsupportedOperationException",
        "java/lang/ClassNotFoundException",
        "java/lang/NoClassDefFoundError",
        "java/lang/UnsatisfiedLinkError",
    };

    for (const char* name : throwableClasses) {
        auto clazz = vm->findClass(name);
        if (!clazz)
            continue;
        clazz->HookInstanceFunction(&frame.getJniEnv(), "setStackTrace", makeSetStackTraceHook());
        clazz->HookInstanceFunction(&frame.getJniEnv(), "getStackTrace", makeGetStackTraceHook());
        verbose("JBRIDGE", "setStackTrace/getStackTrace hooked on %s", name);
    }
}
