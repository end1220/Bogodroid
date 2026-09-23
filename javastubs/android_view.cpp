#include "toml++/toml.hpp"
extern toml::table config;

#include "android.h"
#include "baron/baron.h"
#include "bd_video.h"
#include "device_display.h"
#include "javac.h"
#include "logging.h"
#include <chrono>
#include <fstream>
#include <input_backend.h>
#include <inttypes.h>
#include <mutex>
#include <pthread.h>

///// SurfaceTexture / Surface
//
// Unity renders Android video through a SurfaceTexture feeding a
// GL_TEXTURE_EXTERNAL_OES texture. Nothing on this device fills that buffer
// queue, so javastubs/bd_video.cpp publishes the frames the MediaCodec thunk
// decodes and the GL layer redirects the external bind to its own
// GL_TEXTURE_2D. Everything below is bookkeeping for that bridge.

jnivm::android::graphics::SurfaceTexture::SurfaceTexture(int texture)
    : texture_name(texture)
{
    bd_video::surface_texture_created(texture);
}

void jnivm::android::graphics::SurfaceTexture::setOnFrameAvailableListener(
    std::shared_ptr<OnFrameAvailableListener> value)
{
    listener = std::move(value);
    // Hand the bridge a closure rather than a raw pointer: jnivm's Object is
    // enable_shared_from_this, so this is a real shared_ptr and the media thread
    // can keep it for as long as it needs.
    if (listener) {
        auto self = std::static_pointer_cast<SurfaceTexture>(weak_from_this().lock());
        auto callback = listener;
        bd_video::register_sink(texture_name,
                                [self, callback]() { callback->onFrameAvailable(self); });
    } else {
        // Unity nulls the listener when it retires a clip's video pipeline. Drop
        // the sink with it: the bridge must not call onFrameAvailable on a proxy
        // whose owner is being destroyed (that lands as a pure virtual call).
        bd_video::clear_sink(texture_name);
    }
}

void jnivm::android::graphics::SurfaceTexture::setDefaultBufferSize(
    int value_width, int value_height)
{
    width = value_width;
    height = value_height;
    bd_video::surface_texture_buffer_size(texture_name, value_width, value_height);
}

void jnivm::android::graphics::SurfaceTexture::getTransformMatrix(
    std::shared_ptr<FakeJni::JFloatArray> matrix)
{
    // Identity: a decoder-filled SurfaceTexture without crop has an identity
    // transform. The STUB-MISS default is a zeroed array, which collapses every
    // UV onto one texel - a black or single-colour video quad.
    if (!matrix)
        return;
    for (int i = 0; i < 16; i++)
        (*matrix)[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    static uint64_t calls = 0;
    if (++calls <= 3 || (calls % 300) == 0)
        BD_LOG("VIDEO", "getTransformMatrix #%llu -> identity (size=%d)",
               (unsigned long long)calls, matrix->getSize());
}

long jnivm::android::graphics::SurfaceTexture::getTimestamp()
{
    // Unity uses this as the presented-frame time; frames are published as fast
    // as they decode, so a monotonic microsecond clock is the honest answer.
    static const auto start = std::chrono::steady_clock::now();
    return (long)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

void jnivm::android::graphics::SurfaceTexture::attachToGLContext(int texture)
{
    BD_LOG("VIDEO", "attachToGLContext(%d) texture=%d", texture, texture_name);
    texture_name = texture;
}

void jnivm::android::graphics::SurfaceTexture::detachFromGLContext()
{
    BD_LOG("VIDEO", "detachFromGLContext texture=%d", texture_name);
}

void jnivm::android::graphics::SurfaceTexture::updateTexImage()
{
    bd_video::surface_texture_update_tex_image(texture_name);
}

void jnivm::android::graphics::SurfaceTexture::release()
{
    bd_video::surface_texture_released(texture_name);
    listener.reset();
}

jnivm::android::view::Surface::Surface(
    std::shared_ptr<jnivm::android::graphics::SurfaceTexture> texture)
{
    BD_LOG("VIDEO", "Surface(SurfaceTexture=%p texture=%d)",
           (void*)texture.get(), texture ? texture->texture_name : 0);
    if (texture)
        bd_video::surface_texture_attached(texture->texture_name);
}

///// Display

int jnivm::android::view::Display::getDisplayId()
{
    return 1;
}

int jnivm::android::view::Display::getRotation()
{
    return config["device"]["displayRotation"].value_or<int>(0);
}

int jnivm::android::view::Display::getWidth()
{
    return bd_device_display_width();
}

int jnivm::android::view::Display::getHeight()
{
    return bd_device_display_height();
}

float jnivm::android::view::Display::getRefreshRate()
{
    return bd_device_display_refresh_rate();
}

long jnivm::android::view::Display::getAppVsyncOffsetNanos() { return 0; }
long jnivm::android::view::Display::getPresentationDeadlineNanos() { return 0; }

void jnivm::android::view::Display::getRealMetrics(std::shared_ptr<jnivm::android::util::DisplayMetrics> metrics)
{
    if (!metrics) {
        // [BD] Unity fills this argument by resolving <init> on
        // android/util/DisplayMetrics itself and new-ing one. A null pointer
        // means that lookup missed and the call handed back a default -- and
        // since the very next thing this function does is write through it,
        // that used to end in a SIGSEGV at fault addr 0x58 before the game
        // ever got a frame on screen. Dump what the class actually registered
        // so the registration and the lookup key can be compared directly.
        BD_LOG("JBRIDGE", "getRealMetrics: metrics is NULL");
        auto env = jnivm::ENV::FromJNIEnv(&FakeJni::JniEnvContext().getJniEnv());
        auto cl = env->GetClass<jnivm::android::util::DisplayMetrics>(
            "android/util/DisplayMetrics");
        if (cl) {
            std::lock_guard<std::mutex> lock(cl->mtx);
            BD_LOG("JBRIDGE", "  DisplayMetrics class=%p prefix='%s': %zu methods, %zu fields",
                   (void*)cl.get(), cl->nativeprefix.c_str(),
                   cl->methods.size(), cl->fields.size());
            for (auto& m : cl->methods)
                BD_LOG("JBRIDGE", "   reg %p name='%s' sig='%s' static=%d native=%p handle=%p",
                       (void*)m.get(), m->name.c_str(), m->signature.c_str(), (int)m->_static,
                       m->native, (void*)m->nativehandle.get());
        }
        return;
    }

    metrics->widthPixels = bd_device_display_width();
    metrics->heightPixels = bd_device_display_height();
    metrics->densityDpi = config["device"]["displayDpi"].value_or<int>(100);
}

std::shared_ptr<jnivm::Array<jnivm::android::view::DisplayMode>> jnivm::android::view::Display::getSupportedModes()
{
    verbose("JBRIDGE", "App requests Display Modes.... ");
    auto array = std::make_shared<FakeJni::JArray<jnivm::android::view::DisplayMode>>(1);
    (*array)[0] = std::make_shared<jnivm::android::view::DisplayMode>();
    return array;
}

bool jnivm::android::view::Display::isWideColorGamut()
{
    return false;
}

bool jnivm::android::view::Display::isHdr()
{
    return false;
}

std::shared_ptr<FakeJni::JString> jnivm::android::view::Display::getName()
{
    return std::make_shared<FakeJni::JString>("Built-in Screen");
}

///// WindowManager

std::shared_ptr<jnivm::android::view::Display>
jnivm::android::view::WindowManager::getDefaultDisplay()
{
    BD_LOG("JBRIDGE", "WindowManager.getDefaultDisplay()");
    return std::make_shared<jnivm::android::view::Display>();
}

///// Display$Mode

int jnivm::android::view::DisplayMode::getPhysicalWidth()
{
    return bd_device_display_width();
}

int jnivm::android::view::DisplayMode::getPhysicalHeight()
{
    return bd_device_display_height();
}

float jnivm::android::view::DisplayMode::getRefreshRate()
{
    return bd_device_display_refresh_rate();
}

///// InputDevice

int jnivm::android::view::InputDevice::getSources()
{
    return this->source;
}

int jnivm::android::view::InputDevice::getId()
{
    return this->id;
}

int jnivm::android::view::InputDevice::getVendorId()
{
    return this->vendor;
}

int jnivm::android::view::InputDevice::getProductId()
{
    return this->product;
}

std::shared_ptr<FakeJni::JString> jnivm::android::view::InputDevice::getName()
{
    return this->name;
}

std::shared_ptr<FakeJni::JString> jnivm::android::view::InputDevice::getDescriptorString()
{
    return this->name;
}

bool jnivm::android::view::InputDevice::isVirtual()
{
    return false;
}

std::shared_ptr<jnivm::java::util::List> jnivm::android::view::InputDevice::getMotionRanges()
{
    verbose("InputDevice", "getMotionRanges() called for device %d. Returning %zu ranges.", id, motionRanges.size());
    auto list = std::make_shared<jnivm::java::util::ArrayList>();
    for (const auto& range : motionRanges) {
        list->add(range);
    }
    return list;
}

std::shared_ptr<jnivm::android::view::MotionRange> jnivm::android::view::InputDevice::getMotionRange(int axis)
{
    for (const auto& range : motionRanges) {
        if (range && range->axis == axis)
            return range;
    }
    return nullptr;
}

std::shared_ptr<jnivm::android::view::InputDevice> jnivm::android::view::InputDevice::getDevice(int device)
{
    return InputBackend::instance().getDevice(device);
}

std::shared_ptr<FakeJni::JArray<int>> jnivm::android::view::InputDevice::getDeviceIds()
{
    return InputBackend::instance().getDeviceIds();
}

void jnivm::android::view::InputDevice::addMotionRange(int axis, int src, float min, float max, float flat, float fuzz)
{
    motionRanges.push_back(std::make_shared<MotionRange>(axis, src, min, max, flat, fuzz));
}

///// InputEvent

long jnivm::android::view::InputEvent::getEventTime()
{
    return this->timestamp;
}

std::shared_ptr<jnivm::android::view::InputDevice> jnivm::android::view::InputEvent::getDevice()
{
    return this->device;
}

int jnivm::android::view::InputEvent::getDeviceId()
{
    return this->device.get()->getId();
}

int jnivm::android::view::InputEvent::getSource()
{
    return this->device.get()->getSources();
}

///// KeyEvent

int jnivm::android::view::KeyEvent::getKeyCode()
{
    BD_LOG("INPUT", "KeyEvent.getKeyCode -> %d action=%d device=%d source=0x%x",
           this->keyCode, this->action, this->getDeviceId(), this->getSource());
    return this->keyCode;
}

int jnivm::android::view::KeyEvent::getMetaState()
{
    return this->state;
}

int jnivm::android::view::KeyEvent::getAction()
{
    BD_LOG("INPUT", "KeyEvent.getAction -> %d keycode=%d",
           this->action, this->keyCode);
    return this->action;
}

long jnivm::android::view::KeyEvent::getEventTime()
{
    return this->timestamp;
}

long jnivm::android::view::KeyEvent::getDownTime()
{
    return this->downTime;
}

int jnivm::android::view::KeyEvent::getRepeatCount()
{
    return this->repeatCount;
}

int jnivm::android::view::KeyEvent::getFlags()
{
    return this->flags;
}

int jnivm::android::view::KeyEvent::getScanCode()
{
    return this->scanCode;
}

///// MotionEvent

int jnivm::android::view::MotionEvent::getPointerCount()
{
    // For mouse and joystick, there's always one "pointer".
    return 1;
}

int jnivm::android::view::MotionEvent::getHistorySize()
{
    // We don't generate historical data, so this is always 0.
    return 0;
}

float jnivm::android::view::MotionEvent::getAxisValue(int axis, int pointerIndex)
{
    auto it = axisValues.find(axis);
    return (it != axisValues.end()) ? it->second : 0.0f;
}

int jnivm::android::view::MotionEvent::getToolType(int pointerIndex)
{
    return 3; 
}

float jnivm::android::view::MotionEvent::getX(int pointerIndex)
{
    return this->x;
}

float jnivm::android::view::MotionEvent::getY(int pointerIndex)
{
    return this->y;
}

float jnivm::android::view::MotionEvent::getPressure(int pointerIndex)
{
    (void)pointerIndex;
    return 1.0f;
}

long jnivm::android::view::MotionEvent::getEventTime()
{
    return this->timestamp;
}

int jnivm::android::view::MotionEvent::getButtonState()
{
    return this->buttonState;
}

int jnivm::android::view::MotionEvent::getAction()
{
    return this->action;
}

int jnivm::android::view::MotionEvent::getActionMasked()
{
    return this->action & 0xFF;
}

std::shared_ptr<FakeJni::JString> jnivm::android::view::MotionEvent::axisToString(int axis)
{
    return std::make_shared<FakeJni::JString>("An Axis");
}

std::shared_ptr<jnivm::android::view::MotionEvent> jnivm::android::view::MotionEvent::obtain(std::shared_ptr<MotionEvent> other)
{
    if (!other)
        return nullptr;
    // Create a new MotionEvent by copying the data from the other one.
    auto newEvent = std::make_shared<MotionEvent>(other->device, other->action, other->x, other->y);
    newEvent->axisValues = other->axisValues;
    newEvent->buttonState = other->buttonState;
    return newEvent;
    return other;
}

///// KeyCharacterMap

// --- Singleton implementation for our dummy map ---
static std::shared_ptr<jnivm::android::view::KeyCharacterMap> gDummyMap;
static pthread_once_t gDummyMapOnce = PTHREAD_ONCE_INIT;

static void create_dummy_map_once()
{
    gDummyMap = std::shared_ptr<jnivm::android::view::KeyCharacterMap>(new jnivm::android::view::KeyCharacterMap());
}

// --- Method Implementations ---

std::shared_ptr<jnivm::android::view::KeyCharacterMap> jnivm::android::view::KeyCharacterMap::load(int deviceId)
{
    // Always return the same shared, dummy instance, regardless of device ID.
    pthread_once(&gDummyMapOnce, create_dummy_map_once);
    return gDummyMap;
}

int jnivm::android::view::KeyCharacterMap::get(int keyCode, int metaState)
{

    return 97; // For testing lets just always return lowercase a
}

///// Window

void jnivm::android::view::Window::setFlags(int flag1, int flag2)
{
    verbose("JBRIDGE", "Window.setFlags %d - %d", flag1, flag2);
}

std::shared_ptr<jnivm::android::view::View>
jnivm::android::view::Window::getDecorView()
{
    return std::make_shared<jnivm::android::view::View>();
}

///// View

std::shared_ptr<jnivm::android::view::Display>
jnivm::android::view::View::getDisplay()
{
    return std::make_shared<jnivm::android::view::Display>();
}

int jnivm::android::view::View::getSystemUiVisibility()
{
    return jnivm::android::view::View::SYSTEM_UI_FLAG_FULLSCREEN && SYSTEM_UI_FLAG_HIDE_NAVIGATION;
}

void jnivm::android::view::View::setSystemUiVisibility(int visibility)
{
}

///// Choreographer

static std::shared_ptr<jnivm::android::view::Choreographer> gChoreographerInstance;
static pthread_once_t gChoreographerOnce = PTHREAD_ONCE_INIT;

static void create_global_choreographer_once()
{
    gChoreographerInstance = std::shared_ptr<jnivm::android::view::Choreographer>(new jnivm::android::view::Choreographer());
}

std::shared_ptr<jnivm::android::view::Choreographer> jnivm::android::view::Choreographer::getInstance()
{
    pthread_once(&gChoreographerOnce, create_global_choreographer_once);
    return gChoreographerInstance;
}

jnivm::android::view::Choreographer::Choreographer()
{

    mHandlerThread = std::make_shared<os::HandlerThread>(std::make_shared<FakeJni::JString>("Choreographer"));
    mHandlerThread->start();

    mLooper = mHandlerThread->getLooper();
    mHandler = std::make_shared<os::Handler>(mLooper);

    pthread_mutex_init(&mCallbacksMutex, nullptr);
    mLastVSyncTimeNanos.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
            .count());

    verbose("Choreographer", "Global Instance created with its own HandlerThread.");

    mHandler->post(java::lang::LambdaRunnable::Create([this]() {
        this->watchdogLoop();
    }));
}

jnivm::android::view::Choreographer::~Choreographer()
{
    if (mHandlerThread) {
        mHandlerThread->quit(); // This will stop the looper and thus the watchdog.
        mHandlerThread->join();
    }
    pthread_mutex_destroy(&mCallbacksMutex);
}

// The watchdog loop, now implemented as a recurring Handler task.
void jnivm::android::view::Choreographer::watchdogLoop()
{
    const long long vsyncThresholdNanos = 16'000'000LL; // 1 second

    long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

    long long lastSync = mLastVSyncTimeNanos.load();

    if (now - lastSync > vsyncThresholdNanos) {
        //verbose("Choreographer", "VSync stall detected. Injecting fallback frame.");
        // We are already on the handler thread, so we can call dispatch directly.
        dispatchFrameCallbacks(false);
    }

    // Schedule the next check in 100ms.
    mHandler->postDelayed(java::lang::LambdaRunnable::Create([this]() {
        this->watchdogLoop();
    }),
        2);
}

void jnivm::android::view::Choreographer::postFrameCallback(std::shared_ptr<FrameCallback> callback)
{
    if (!callback)
        return;
    pthread_mutex_lock(&mCallbacksMutex);
    //verbose("Choreographer", "[Thread: %" PRIxPTR "] [Instance: %p] postFrameCallback ENTER. Queue size before: %zu",
    //    (uintptr_t)pthread_self(), this, mCallbacks.size());
    mCallbacks.push_back(callback);
    //verbose("Choreographer", "[Thread: %" PRIxPTR "] [Instance: %p] postFrameCallback EXIT. Queue size after: %zu",
    //    (uintptr_t)pthread_self(), this, mCallbacks.size());
    pthread_mutex_unlock(&mCallbacksMutex);
}

// signalVSync is the entry point for eglSwapBuffers.
void jnivm::android::view::Choreographer::signalVSync()
{
    // Post the work to our handler thread to ensure all dispatches are serialized.
    mHandler->post(java::lang::LambdaRunnable::Create([this]() {
        this->dispatchFrameCallbacks(true);
    }));
}

// The core dispatch logic, with the critical isRealVSync flag.
void jnivm::android::view::Choreographer::dispatchFrameCallbacks(bool isRealVSync)
{
    long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

    // Only a real vsync signal updates the master timestamp.
    if (isRealVSync) {
        mLastVSyncTimeNanos.store(now);
    }

    std::vector<std::shared_ptr<FrameCallback>> callbacksToRun;
    //verbose("Choreographer", "[Thread: %" PRIxPTR "] [Instance: %p] dispatch ENTER (isRealVSync: %s). Queue size before swap: %zu",
    //    (uintptr_t)pthread_self(), this, isRealVSync ? "true" : "false", mCallbacks.size());
    pthread_mutex_lock(&mCallbacksMutex);
    mCallbacks.swap(callbacksToRun);
    pthread_mutex_unlock(&mCallbacksMutex);

    //verbose("Choreographer", "[Thread: %" PRIxPTR "] [Instance: %p] dispatch EXIT. Callbacks to run: %zu. Queue size after swap: %zu",
    //    (uintptr_t)pthread_self(), this, callbacksToRun.size(), mCallbacks.size());

    if (callbacksToRun.empty()) {
        //verbose("Choreographer", "No callbacks to dispatch (isRealVSync: %s)", isRealVSync ? "true" : "false");
        return;
    }

    // verbose("Choreographer", "Dispatching %zu callbacks (isRealVSync: %s)",
    //     callbacksToRun.size(), isRealVSync ? "true" : "false");

    // We are on the handler thread, so we can execute the callbacks directly and synchronously.
    for (const auto& callback : callbacksToRun) {
        callback->doFrame(now);
    }
}

///// ContextThemeWrapper

std::shared_ptr<jnivm::android::content::res::Resources> jnivm::android::view::ContextThemeWrapper::getResources()
{
    return std::make_shared<jnivm::android::content::res::Resources>();
}

///// View Descriptors

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::Display) { FakeJni::Constructor<Display> {} },
    { FakeJni::Function<&Display::getDisplayId> {}, "getDisplayId", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getRotation> {}, "getRotation", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getAppVsyncOffsetNanos> {}, "getAppVsyncOffsetNanos", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getPresentationDeadlineNanos> {}, "getPresentationDeadlineNanos", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getWidth> {}, "getWidth", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getHeight> {}, "getHeight", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getRefreshRate> {}, "getRefreshRate", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getRealMetrics> {}, "getRealMetrics", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getSupportedModes> {}, "getSupportedModes", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::isWideColorGamut> {}, "isWideColorGamut", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::isHdr> {}, "isHdr", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Display::getName> {}, "getName", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::DisplayMode) { FakeJni::Constructor<DisplayMode> {} },
    { FakeJni::Function<&DisplayMode::getPhysicalWidth> {}, "getPhysicalWidth", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&DisplayMode::getPhysicalHeight> {}, "getPhysicalHeight", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&DisplayMode::getRefreshRate> {}, "getRefreshRate", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::graphics::SurfaceTexture)
    { FakeJni::Constructor<SurfaceTexture, int> {} },
    { FakeJni::Function<&SurfaceTexture::setOnFrameAvailableListener> {},
      "setOnFrameAvailableListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::setDefaultBufferSize> {},
      "setDefaultBufferSize", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::getTransformMatrix> {},
      "getTransformMatrix", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::getTimestamp> {},
      "getTimestamp", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::attachToGLContext> {},
      "attachToGLContext", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::detachFromGLContext> {},
      "detachFromGLContext", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::updateTexImage> {},
      "updateTexImage", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&SurfaceTexture::release> {},
      "release", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(
        jnivm::android::graphics::SurfaceTexture::OnFrameAvailableListener)
    { FakeJni::Function<&OnFrameAvailableListener::onFrameAvailable> {},
      "onFrameAvailable", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::Surface)
    { FakeJni::Constructor<Surface> {} },
    { FakeJni::Constructor<
        Surface, std::shared_ptr<jnivm::android::graphics::SurfaceTexture>> {} },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::InputDevice) { FakeJni::Constructor<InputDevice> {} },
    { FakeJni::Function<&InputDevice::getSources> {}, "getSources", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getId> {}, "getId", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getProductId> {}, "getProductId", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getVendorId> {}, "getVendorId", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getName> {}, "getName", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getDescriptorString> {}, "getDescriptor", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::isVirtual> {}, "isVirtual", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getMotionRanges> {}, "getMotionRanges", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getMotionRange> {}, "getMotionRange", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputDevice::getDevice> {}, "getDevice", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&InputDevice::getDeviceIds> {}, "getDeviceIds", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::InputEvent) { FakeJni::Constructor<InputEvent, std::shared_ptr<jnivm::android::view::InputDevice>> {} },
    { FakeJni::Function<&InputEvent::getEventTime> {}, "getEventTime", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputEvent::getDevice> {}, "getDevice", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputEvent::getDeviceId> {}, "getDeviceId", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&InputEvent::getSource> {}, "getSource", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::KeyEvent) { FakeJni::Constructor<KeyEvent, std::shared_ptr<jnivm::android::view::InputDevice>, int, int, int> {} },

    { FakeJni::Field<&KeyEvent::KEYCODE_UNKNOWN> {}, "KEYCODE_UNKNOWN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BACK> {}, "KEYCODE_BACK", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_DPAD_UP> {}, "KEYCODE_DPAD_UP", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_DPAD_DOWN> {}, "KEYCODE_DPAD_DOWN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_DPAD_LEFT> {}, "KEYCODE_DPAD_LEFT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_DPAD_RIGHT> {}, "KEYCODE_DPAD_RIGHT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_DPAD_CENTER> {}, "KEYCODE_DPAD_CENTER", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_VOLUME_UP> {}, "KEYCODE_VOLUME_UP", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_VOLUME_DOWN> {}, "KEYCODE_VOLUME_DOWN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_CAMERA> {}, "KEYCODE_CAMERA", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_ZOOM_IN> {}, "KEYCODE_ZOOM_IN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_ZOOM_OUT> {}, "KEYCODE_ZOOM_OUT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_A> {}, "KEYCODE_BUTTON_A", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_B> {}, "KEYCODE_BUTTON_B", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_X> {}, "KEYCODE_BUTTON_X", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_Y> {}, "KEYCODE_BUTTON_Y", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_START> {}, "KEYCODE_BUTTON_START", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_SELECT> {}, "KEYCODE_BUTTON_SELECT", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_BUTTON_MODE> {}, "KEYCODE_BUTTON_MODE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::KEYCODE_ESCAPE> {}, "KEYCODE_ESCAPE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::ACTION_DOWN> {}, "ACTION_DOWN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::ACTION_UP> {}, "ACTION_UP", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&KeyEvent::ACTION_MULTIPLE> {}, "ACTION_MULTIPLE", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&KeyEvent::getKeyCode> {},     "getKeyCode",     FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getAction> {},      "getAction",      FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getMetaState> {},   "getMetaState",   FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getEventTime> {},   "getEventTime",   FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getDownTime> {},    "getDownTime",    FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getDeviceId> {},    "getDeviceId",    FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getSource> {},      "getSource",      FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getDevice> {},      "getDevice",      FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getRepeatCount> {}, "getRepeatCount", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getFlags> {},       "getFlags",       FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&KeyEvent::getScanCode> {},    "getScanCode",    FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::MotionRange) { FakeJni::Constructor<MotionRange, int, int, float, float, float, float> {} },
    { FakeJni::Function<&MotionRange::getAxis> {}, "getAxis", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getSource> {}, "getSource", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getMin> {}, "getMin", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getMax> {}, "getMax", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getRange> {}, "getRange", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getFlat> {}, "getFlat", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionRange::getFuzz> {}, "getFuzz", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::MotionEvent) { FakeJni::Constructor<MotionEvent, std::shared_ptr<jnivm::android::view::InputDevice>, int, int, int> {} },
    { FakeJni::Function<&MotionEvent::getEventTime> {}, "getEventTime", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getPointerCount> {}, "getPointerCount", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getHistorySize> {}, "getHistorySize", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getAxisValue> {}, "getAxisValue", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getToolType> {}, "getToolType", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getButtonState> {}, "getButtonState", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getAction> {}, "getAction", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getActionMasked> {}, "getActionMasked", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getX> {}, "getX", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getY> {}, "getY", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::getPressure> {}, "getPressure", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&MotionEvent::axisToString> {}, "axisToString", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&MotionEvent::obtain> {}, "obtain", FakeJni::JMethodID::STATIC },

    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::KeyCharacterMap) { FakeJni::Constructor<KeyCharacterMap> {} },
    { FakeJni::Function<&KeyCharacterMap::load> {}, "load", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&KeyCharacterMap::get> {}, "get", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::Window) { FakeJni::Constructor<Window> {} },
    { FakeJni::Function<&Window::setFlags> {}, "setFlags", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Window::getDecorView> {}, "getDecorView", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::WindowManager) { FakeJni::Constructor<WindowManager> {} },
    { FakeJni::Function<&WindowManager::getDefaultDisplay> {}, "getDefaultDisplay", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::View) { FakeJni::Constructor<View> {} },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_IMMERSIVE_STICKY> {}, "SYSTEM_UI_FLAG_IMMERSIVE_STICKY", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_LAYOUT_STABLE> {}, "SYSTEM_UI_FLAG_LAYOUT_STABLE", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN> {}, "SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_HIDE_NAVIGATION> {}, "SYSTEM_UI_FLAG_HIDE_NAVIGATION", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_FULLSCREEN> {}, "SYSTEM_UI_FLAG_FULLSCREEN", FakeJni::JFieldID::STATIC },
    { FakeJni::Field<&View::SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION> {}, "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION", FakeJni::JFieldID::STATIC },
    { FakeJni::Function<&View::getDisplay> {}, "getDisplay", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&View::getSystemUiVisibility> {}, "getSystemUiVisibility", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&View::setSystemUiVisibility> {}, "setSystemUiVisibility", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::SurfaceView) { FakeJni::Constructor<SurfaceView> {} },
    { FakeJni::Function<&InputDevice::getDevice> {}, "getDevice", FakeJni::JMethodID::STATIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::Choreographer) { FakeJni::Function<&Choreographer::getInstance> {}, "getInstance", FakeJni::JMethodID::STATIC },
    { FakeJni::Function<&Choreographer::postFrameCallback> {}, "postFrameCallback", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&Choreographer::dispatchFrameCallbacks> {}, "dispatchFrameCallbacks", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::Choreographer::FrameCallback) { FakeJni::Function<&FrameCallback::doFrame> {}, "doFrame", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

    BEGIN_NATIVE_DESCRIPTOR(jnivm::android::view::ContextThemeWrapper) { FakeJni::Constructor<ContextThemeWrapper> {} },
    { FakeJni::Function<&ContextThemeWrapper::getResources> {}, "getResources", FakeJni::JMethodID::PUBLIC },
    END_NATIVE_DESCRIPTOR

///// AlertDialog$Builder

jnivm::android::app::AlertDialogBuilder::AlertDialogBuilder(std::shared_ptr<jnivm::android::content::Context> context)
{
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setTitle(std::shared_ptr<jnivm::CharSequence> title)
{
    if (auto str = title->toString()) {
        warning("AlertDialog title: %s\n", str->c_str());
    }
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setMessage(std::shared_ptr<jnivm::CharSequence> message)
{
    if (auto str = message->toString()) {
        warning("AlertDialog message: %s\n", str->c_str());
    }
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setPositiveButton(std::shared_ptr<jnivm::CharSequence> text, std::shared_ptr<jnivm::android::app::DialogInterfaceOnClickListener> listener)
{
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setNegativeButton(std::shared_ptr<jnivm::CharSequence> text, std::shared_ptr<jnivm::android::app::DialogInterfaceOnClickListener> listener)
{
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setOnCancelListener(std::shared_ptr<jnivm::android::app::DialogInterfaceOnCancelListener> listener)
{
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialogBuilder> jnivm::android::app::AlertDialogBuilder::setView(std::shared_ptr<jnivm::android::view::View> view)
{
    return std::static_pointer_cast<jnivm::android::app::AlertDialogBuilder>(shared_from_this());
}

std::shared_ptr<jnivm::android::app::AlertDialog> jnivm::android::app::AlertDialogBuilder::show()
{
    return std::make_shared<jnivm::android::app::AlertDialog>();
}

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::DialogInterface)
    { FakeJni::Constructor<DialogInterface> {} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::DialogInterfaceOnClickListener)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::DialogInterfaceOnCancelListener)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::AlertDialog)
    { FakeJni::Constructor<AlertDialog> {} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(jnivm::android::app::AlertDialogBuilder)
    { FakeJni::Constructor<AlertDialogBuilder, std::shared_ptr<jnivm::android::content::Context>> {} },
    { FakeJni::Function<&AlertDialogBuilder::setTitle> {}, "setTitle", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::setMessage> {}, "setMessage", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::setPositiveButton> {}, "setPositiveButton", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::setNegativeButton> {}, "setNegativeButton", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::setOnCancelListener> {}, "setOnCancelListener", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::setView> {}, "setView", FakeJni::JMethodID::PUBLIC },
    { FakeJni::Function<&AlertDialogBuilder::show> {}, "show", FakeJni::JMethodID::PUBLIC },
END_NATIVE_DESCRIPTOR
