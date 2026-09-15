// SPDX-License-Identifier: GPL-3.0-or-later
// Substantial additions Copyright (c) 2025-2026 jenny92-tech <jennyliu90223@gmail.com>
// (Upstream attribution preserved via git log.)
#ifndef __ANDROID_H__
#define __ANDROID_H__

#include "alooper.h"
#include "baron/baron.h"
#include "javac.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

void InitJNIAndroidClasses(FakeJni::Jvm* vm);

namespace jnivm {
namespace android {
    // Android's register*Listener APIs all take a java-side android.os.Handler
    // to pick the callback thread. os/ is defined further down, so forward
    // declare it here rather than reordering the file.
    namespace os {
        class Handler;
    }

    namespace net {
        class Uri : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/net/Uri")
            static std::shared_ptr<FakeJni::JString> encode(std::shared_ptr<FakeJni::JString> string);
            static std::shared_ptr<FakeJni::JString> decode(std::shared_ptr<FakeJni::JString> string);
        };

    }

    namespace util {
        class DisplayMetrics : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/util/DisplayMetrics")
            int widthPixels = 0;
            int heightPixels = 0;
            int densityDpi = 0;
        };
    }
    namespace view {
        // View is defined below; View$OnLayoutChangeListener names it in its
        // callback signature, so forward declare it here.
        class View;

        class DisplayMode : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/Display$Mode")
            int getPhysicalWidth();
            int getPhysicalHeight();
            float getRefreshRate();
        };

        class Display : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/Display")
            int getDisplayId();
            int getRotation();
            int getWidth();
            int getHeight();
            float getRefreshRate();
            long getAppVsyncOffsetNanos();
            long getPresentationDeadlineNanos();
            void getRealMetrics(std::shared_ptr<jnivm::android::util::DisplayMetrics> metrics);
            std::shared_ptr<jnivm::Array<jnivm::android::view::DisplayMode>> getSupportedModes();
            bool isWideColorGamut();
            bool isHdr();
            std::shared_ptr<FakeJni::JString> getName();
        };
        class Surface : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/Surface")
        };

        // android.view.View$OnLayoutChangeListener. Unity 6 attaches one to the
        // surface view and reads the settled view size out of the callback; the
        // interface must exist as a registered class or GetMethodID cannot bind
        // the parameter type and the call falls to the STUB-MISS path. Declared
        // here so View can name it in the add/remove signatures, like the real
        // class (the listener type belongs to View, not to SurfaceView).
        class ViewOnLayoutChangeListener : public virtual FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/View$OnLayoutChangeListener")
            virtual void onLayoutChange(std::shared_ptr<jnivm::android::view::View> view,
                                        int left, int top, int right, int bottom,
                                        int oldLeft, int oldTop, int oldRight, int oldBottom);
        };

        class View : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/View")
            inline static int SYSTEM_UI_FLAG_IMMERSIVE_STICKY = 4096;
            inline static int SYSTEM_UI_FLAG_LAYOUT_STABLE = 256;
            inline static int SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN = 1024;
            inline static int SYSTEM_UI_FLAG_HIDE_NAVIGATION = 2;
            inline static int SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION = 512;
            inline static int SYSTEM_UI_FLAG_FULLSCREEN = 4;
            int getSystemUiVisibility();
            void setSystemUiVisibility(int visibility);
            std::shared_ptr<jnivm::android::view::Display> getDisplay();

            // These live on View in the real framework and Unity 6 looks them
            // up on android/view/View (not on the SurfaceView it passes), so
            // declaring them on SurfaceView leaves the call unresolved:
            //   Constructed Unresolved symbol, Class=`android/view/View`,
            //   Method=`addOnLayoutChangeListener`
            // Hold the listener (Android keeps a strong reference too) but do
            // not fire it: the Java player is stubbed out under this loader, so
            // the callback would re-enter native code that expects a live
            // UnityPlayer. The size Unity cares about is already published
            // through the EGL surface.
            void addOnLayoutChangeListener(
                std::shared_ptr<jnivm::android::view::ViewOnLayoutChangeListener> listener);
            void removeOnLayoutChangeListener(
                std::shared_ptr<jnivm::android::view::ViewOnLayoutChangeListener> listener);

        private:
            std::vector<std::shared_ptr<jnivm::android::view::ViewOnLayoutChangeListener>> layoutListeners;
        };

        class SurfaceView : public View {
        public:
            DEFINE_CLASS_NAME("android/view/SurfaceView", View)
        };

        class Window : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/Window")
            void setFlags(int flag1, int flag2);
            std::shared_ptr<jnivm::android::view::View> getDecorView();
        };

        // Unity 2022 queries Activity.getWindowManager().getDefaultDisplay()
        // during the first nativeRender. A STUB-MISS here returns a phantom
        // Invalid jobject; the next GetMethodID/CallObjectMethod on it is a
        // null deref inside libunity.so.
        class WindowManager : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/WindowManager")

            // android/view/WindowManager$LayoutParams. Unity 6 reads
            // FLAG_KEEP_SCREEN_ON (and the other window flags) with
            // GetStaticIntField after Class.forName(), so the class has to be
            // registered with the constants as real static fields.
            class LayoutParams : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/view/WindowManager$LayoutParams")

                inline static int FLAG_ALLOW_LOCK_WHILE_SCREEN_ON = 0x00000001;
                inline static int FLAG_DIM_BEHIND = 0x00000002;
                inline static int FLAG_NOT_FOCUSABLE = 0x00000008;
                inline static int FLAG_NOT_TOUCHABLE = 0x00000010;
                inline static int FLAG_NOT_TOUCH_MODAL = 0x00000020;
                inline static int FLAG_KEEP_SCREEN_ON = 0x00000080;
                inline static int FLAG_LAYOUT_IN_SCREEN = 0x00000100;
                inline static int FLAG_LAYOUT_NO_LIMITS = 0x00000200;
                inline static int FLAG_FULLSCREEN = 0x00000400;
                inline static int FLAG_FORCE_NOT_FULLSCREEN = 0x00000800;
                inline static int FLAG_SECURE = 0x00002000;
                inline static int FLAG_SCALED = 0x00004000;
                inline static int FLAG_IGNORE_CHEEK_PRESSES = 0x00008000;
                inline static int FLAG_LAYOUT_INSET_DECOR = 0x00010000;
                inline static int FLAG_ALT_FOCUSABLE_IM = 0x00020000;
                inline static int FLAG_WATCH_OUTSIDE_TOUCH = 0x00040000;
                inline static int FLAG_SHOW_WHEN_LOCKED = 0x00080000;
                inline static int FLAG_SHOW_WALLPAPER = 0x00100000;
                inline static int FLAG_TURN_SCREEN_ON = 0x00200000;
                inline static int FLAG_DISMISS_KEYGUARD = 0x00400000;
                inline static int FLAG_SPLIT_TOUCH = 0x00800000;
                inline static int FLAG_HARDWARE_ACCELERATED = 0x01000000;
                inline static int FLAG_LAYOUT_IN_OVERSCAN = 0x02000000;
                inline static int FLAG_TRANSLUCENT_STATUS = 0x04000000;
                inline static int FLAG_TRANSLUCENT_NAVIGATION = 0x08000000;
                inline static int FLAG_LOCAL_FOCUS_MODE = 0x10000000;
                inline static int FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS = (int)0x80000000u;
            };

            std::shared_ptr<jnivm::android::view::Display> getDefaultDisplay();
        };

        class MotionRange : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/InputDevice$MotionRange")

            int axis, source;
            float min, max, flat, fuzz;

            MotionRange(int ax, int src, float mn, float mx, float fl, float fz)
                : axis(ax)
                , source(src)
                , min(mn)
                , max(mx)
                , flat(fl)
                , fuzz(fz)
            {
            }

            int getAxis() { return axis; }
            int getSource() { return source; }
            float getMin() { return min; }
            float getMax() { return max; }
            float getRange() { return max - min; }
            float getFlat() { return flat; }
            float getFuzz() { return fuzz; }
        };

        class InputDevice : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/InputDevice")

            static inline int SOURCE_KEYBOARD = 0x00000101;
            static inline int SOURCE_DPAD = 0x00000201;
            static inline int SOURCE_GAMEPAD = 0x00000401;
            static inline int SOURCE_JOYSTICK = 0x01000010;
            static inline int SOURCE_MOUSE = 0x00002002;
            static inline int SOURCE_TOUCHSCREEN = 0x00001002;

            int id = 0;
            int vendor = 0x045e; // Microsoft
            int product = 0x028e; // Xbox 360 controller
            std::shared_ptr<FakeJni::JString> name = std::make_shared<FakeJni::JString>("Microsoft X-Box 360 pad");
            int source = SOURCE_GAMEPAD; // Gamepad
            std::vector<std::shared_ptr<MotionRange>> motionRanges;

            int getSources();
            int getId();
            int getProductId();
            int getVendorId();
            std::shared_ptr<FakeJni::JString> getName();
            std::shared_ptr<FakeJni::JString> getDescriptorString(); // Actually getDescriptor() but that's already taken
            bool isVirtual();
            std::shared_ptr<java::util::List> getMotionRanges();
            std::shared_ptr<MotionRange> getMotionRange(int axis);
            static std::shared_ptr<jnivm::android::view::InputDevice> getDevice(int device);
            static std::shared_ptr<FakeJni::JArray<int>> getDeviceIds();

            void addMotionRange(int axis, int src, float min, float max, float flat, float fuzz); // Helper
        };

        class InputEvent : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/InputEvent")
            std::shared_ptr<jnivm::android::view::InputDevice> device;
            long timestamp = duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            InputEvent(std::shared_ptr<jnivm::android::view::InputDevice> dev)
                : device(dev)
            {
            }
            int getDeviceId();
            int getSource();
            long getEventTime();
            std::shared_ptr<jnivm::android::view::InputDevice> getDevice();
        };

        class KeyEvent : public InputEvent {
        public:
            DEFINE_CLASS_NAME("android/view/KeyEvent", jnivm::android::view::InputEvent)

            static inline int KEYCODE_UNKNOWN = 0;
            static inline int KEYCODE_SOFT_LEFT = 1;
            static inline int KEYCODE_SOFT_RIGHT = 2;
            static inline int KEYCODE_HOME = 3;
            static inline int KEYCODE_BACK = 4;
            static inline int KEYCODE_CALL = 5;
            static inline int KEYCODE_ENDCALL = 6;
            static inline int KEYCODE_0 = 7;
            static inline int KEYCODE_1 = 8;
            static inline int KEYCODE_2 = 9;
            static inline int KEYCODE_3 = 10;
            static inline int KEYCODE_4 = 11;
            static inline int KEYCODE_5 = 12;
            static inline int KEYCODE_6 = 13;
            static inline int KEYCODE_7 = 14;
            static inline int KEYCODE_8 = 15;
            static inline int KEYCODE_9 = 16;
            static inline int KEYCODE_STAR = 17;
            static inline int KEYCODE_POUND = 18;
            static inline int KEYCODE_DPAD_UP = 19;
            static inline int KEYCODE_DPAD_DOWN = 20;
            static inline int KEYCODE_DPAD_LEFT = 21;
            static inline int KEYCODE_DPAD_RIGHT = 22;
            static inline int KEYCODE_DPAD_CENTER = 23;
            static inline int KEYCODE_VOLUME_UP = 24;
            static inline int KEYCODE_VOLUME_DOWN = 25;
            static inline int KEYCODE_POWER = 26;
            static inline int KEYCODE_CAMERA = 27;
            static inline int KEYCODE_CLEAR = 28;
            static inline int KEYCODE_A = 29;
            static inline int KEYCODE_B = 30;
            static inline int KEYCODE_C = 31;
            static inline int KEYCODE_D = 32;
            static inline int KEYCODE_E = 33;
            static inline int KEYCODE_F = 34;
            static inline int KEYCODE_G = 35;
            static inline int KEYCODE_H = 36;
            static inline int KEYCODE_I = 37;
            static inline int KEYCODE_J = 38;
            static inline int KEYCODE_K = 39;
            static inline int KEYCODE_L = 40;
            static inline int KEYCODE_M = 41;
            static inline int KEYCODE_N = 42;
            static inline int KEYCODE_O = 43;
            static inline int KEYCODE_P = 44;
            static inline int KEYCODE_Q = 45;
            static inline int KEYCODE_R = 46;
            static inline int KEYCODE_S = 47;
            static inline int KEYCODE_T = 48;
            static inline int KEYCODE_U = 49;
            static inline int KEYCODE_V = 50;
            static inline int KEYCODE_W = 51;
            static inline int KEYCODE_X = 52;
            static inline int KEYCODE_Y = 53;
            static inline int KEYCODE_Z = 54;
            static inline int KEYCODE_COMMA = 55;
            static inline int KEYCODE_PERIOD = 56;
            static inline int KEYCODE_ALT_LEFT = 57;
            static inline int KEYCODE_ALT_RIGHT = 58;
            static inline int KEYCODE_SHIFT_LEFT = 59;
            static inline int KEYCODE_SHIFT_RIGHT = 60;
            static inline int KEYCODE_TAB = 61;
            static inline int KEYCODE_SPACE = 62;
            static inline int KEYCODE_SYM = 63;
            static inline int KEYCODE_EXPLORER = 64;
            static inline int KEYCODE_ENVELOPE = 65;
            static inline int KEYCODE_ENTER = 66;
            static inline int KEYCODE_DEL = 67;
            static inline int KEYCODE_GRAVE = 68;
            static inline int KEYCODE_MINUS = 69;
            static inline int KEYCODE_EQUALS = 70;
            static inline int KEYCODE_LEFT_BRACKET = 71;
            static inline int KEYCODE_RIGHT_BRACKET = 72;
            static inline int KEYCODE_BACKSLASH = 73;
            static inline int KEYCODE_SEMICOLON = 74;
            static inline int KEYCODE_APOSTROPHE = 75;
            static inline int KEYCODE_SLASH = 76;
            static inline int KEYCODE_AT = 77;
            static inline int KEYCODE_NUM = 78;
            static inline int KEYCODE_HEADSETHOOK = 79;
            static inline int KEYCODE_FOCUS = 80;
            static inline int KEYCODE_PLUS = 81;
            static inline int KEYCODE_MENU = 82;
            static inline int KEYCODE_NOTIFICATION = 83;
            static inline int KEYCODE_SEARCH = 84;
            static inline int KEYCODE_MEDIA_PLAY_PAUSE = 85;
            static inline int KEYCODE_MEDIA_STOP = 86;
            static inline int KEYCODE_MEDIA_NEXT = 87;
            static inline int KEYCODE_MEDIA_PREVIOUS = 88;
            static inline int KEYCODE_MEDIA_REWIND = 89;
            static inline int KEYCODE_MEDIA_FAST_FORWARD = 90;
            static inline int KEYCODE_MUTE = 91;
            static inline int KEYCODE_PAGE_UP = 92;
            static inline int KEYCODE_PAGE_DOWN = 93;
            static inline int KEYCODE_PICTSYMBOLS = 94;
            static inline int KEYCODE_SWITCH_CHARSET = 95;
            static inline int KEYCODE_BUTTON_A = 96;
            static inline int KEYCODE_BUTTON_B = 97;
            static inline int KEYCODE_BUTTON_C = 98;
            static inline int KEYCODE_BUTTON_X = 99;
            static inline int KEYCODE_BUTTON_Y = 100;
            static inline int KEYCODE_BUTTON_Z = 101;
            static inline int KEYCODE_BUTTON_L1 = 102;
            static inline int KEYCODE_BUTTON_R1 = 103;
            static inline int KEYCODE_BUTTON_L2 = 104;
            static inline int KEYCODE_BUTTON_R2 = 105;
            static inline int KEYCODE_BUTTON_THUMBL = 106;
            static inline int KEYCODE_BUTTON_THUMBR = 107;
            static inline int KEYCODE_BUTTON_START = 108;
            static inline int KEYCODE_BUTTON_SELECT = 109;
            static inline int KEYCODE_BUTTON_MODE = 110;
            static inline int KEYCODE_ESCAPE = 111;
            static inline int KEYCODE_FORWARD_DEL = 112;
            static inline int KEYCODE_CTRL_LEFT = 113;
            static inline int KEYCODE_CTRL_RIGHT = 114;
            static inline int KEYCODE_CAPS_LOCK = 115;
            static inline int KEYCODE_SCROLL_LOCK = 116;
            static inline int KEYCODE_SYSRQ = 120;
            static inline int KEYCODE_BREAK = 121;
            static inline int KEYCODE_MOVE_HOME = 122;
            static inline int KEYCODE_MOVE_END = 123;
            static inline int KEYCODE_INSERT = 124;
            static inline int KEYCODE_F1 = 131;
            static inline int KEYCODE_F2 = 132;
            static inline int KEYCODE_F3 = 133;
            static inline int KEYCODE_F4 = 134;
            static inline int KEYCODE_F5 = 135;
            static inline int KEYCODE_F6 = 136;
            static inline int KEYCODE_F7 = 137;
            static inline int KEYCODE_F8 = 138;
            static inline int KEYCODE_F9 = 139;
            static inline int KEYCODE_F10 = 140;
            static inline int KEYCODE_F11 = 141;
            static inline int KEYCODE_F12 = 142;
            static inline int KEYCODE_NUM_LOCK = 143;
            static inline int KEYCODE_NUMPAD_0 = 144;
            static inline int KEYCODE_NUMPAD_1 = 145;
            static inline int KEYCODE_NUMPAD_2 = 146;
            static inline int KEYCODE_NUMPAD_3 = 147;
            static inline int KEYCODE_NUMPAD_4 = 148;
            static inline int KEYCODE_NUMPAD_5 = 149;
            static inline int KEYCODE_NUMPAD_6 = 150;
            static inline int KEYCODE_NUMPAD_7 = 151;
            static inline int KEYCODE_NUMPAD_8 = 152;
            static inline int KEYCODE_NUMPAD_9 = 153;
            static inline int KEYCODE_NUMPAD_DIVIDE = 154;
            static inline int KEYCODE_NUMPAD_MULTIPLY = 155;
            static inline int KEYCODE_NUMPAD_SUBTRACT = 156;
            static inline int KEYCODE_NUMPAD_ADD = 157;
            static inline int KEYCODE_NUMPAD_DOT = 158;
            static inline int KEYCODE_NUMPAD_COMMA = 159;
            static inline int KEYCODE_NUMPAD_ENTER = 160;
            static inline int KEYCODE_NUMPAD_EQUALS = 161;
            static inline int KEYCODE_NUMPAD_LEFT_PAREN = 162;
            static inline int KEYCODE_NUMPAD_RIGHT_PAREN = 163;

            static inline int MAX_KEYCODE = 164;
            static inline int META_ALT_ON = 2;
            static inline int META_ALT_LEFT_ON = 16;
            static inline int META_ALT_RIGHT_ON = 32;
            static inline int META_SHIFT_ON = 1;
            static inline int META_SHIFT_LEFT_ON = 64;
            static inline int META_SHIFT_RIGHT_ON = 128;
            static inline int META_SYM_ON = 4;
            static inline int FLAG_WOKE_HERE = 1;
            static inline int FLAG_SOFT_KEYBOARD = 2;
            static inline int FLAG_KEEP_TOUCH_MODE = 4;
            static inline int FLAG_FROM_SYSTEM = 8;
            static inline int FLAG_EDITOR_ACTION = 16;
            static inline int FLAG_CANCELED = 32;
            static inline int FLAG_VIRTUAL_HARD_KEY = 64;
            static inline int FLAG_LONG_PRESS = 128;
            static inline int FLAG_CANCELED_LONG_PRESS = 256;
            static inline int FLAG_TRACKING = 512;

            static inline int ACTION_DOWN = 0;
            static inline int ACTION_UP = 1;
            static inline int ACTION_MULTIPLE = 2;

            int action;
            int keyCode;
            int state;          // metaState
            // Standard Android KeyEvent fields. Apps and Unity InputSystem
            // routinely call these getters; if we leave them at 0, holdTime
            // (= eventTime - downTime) becomes ~eventTime which trips most
            // long-press / repeat logic in games. See Android docs:
            // https://developer.android.com/reference/android/view/KeyEvent
            long downTime    = 0;
            int  repeatCount = 0;
            int  flags       = 0;
            int  scanCode    = 0;
            long timestamp = duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            KeyEvent(std::shared_ptr<jnivm::android::view::InputDevice> dev, int act, int code, int st)
                : InputEvent(dev)
                , action(act)
                , keyCode(code)
                , state(st)
            {
            }

            int  getKeyCode();
            int  getAction();
            int  getMetaState();
            long getEventTime();
            long getDownTime();
            int  getRepeatCount();
            int  getFlags();
            int  getScanCode();
        };

        class MotionEvent : public InputEvent {
        public:
            DEFINE_CLASS_NAME("android/view/MotionEvent", jnivm::android::view::InputEvent)

            static inline int AXIS_X = 0;
            static inline int AXIS_Y = 1;
            static inline int AXIS_Z = 11;
            static inline int AXIS_RZ = 14;
            static inline int AXIS_HAT_X = 15;       // D-pad horizontal hat axis
            static inline int AXIS_HAT_Y = 16;       // D-pad vertical hat axis
            static inline int AXIS_LTRIGGER = 17;
            static inline int AXIS_RTRIGGER = 18;
            static inline int AXIS_BRAKE = 23;
            static inline int AXIS_GAS = 22;

            static inline int ACTION_DOWN = 0;
            static inline int ACTION_UP = 1;
            static inline int ACTION_MOVE = 2;
            static inline int ACTION_HOVER_MOVE = 7;
            static inline int ACTION_HOVER_ENTER = 9;
            static inline int ACTION_HOVER_EXIT = 10;
            static inline int ACTION_BUTTON_PRESS = 11;
            static inline int ACTION_BUTTON_RELEASE = 12;

            // Mouse buttons
            static inline int BUTTON_PRIMARY = 1;
            static inline int BUTTON_SECONDARY = 2;
            static inline int BUTTON_TERTIARY = 4; // Middle

            int action;
            float x, y;
            int buttonState = 0;

            std::unordered_map<int, float> axisValues;

            long timestamp = duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            MotionEvent(std::shared_ptr<jnivm::android::view::InputDevice> dev, int act, float pX, float pY)
                : InputEvent(dev)
                , action(act)
                , x(pX)
                , y(pY)
            {
            }
            long getEventTime();
            int getPointerCount();
            int getHistorySize();
            int getButtonState();
            int getToolType(int pointerIndex);
            int getAction();
            int getActionMasked();
            float getAxisValue(int axis, int pointerIndex);
            float getX(int pointerIndex);
            float getY(int pointerIndex);
            float getPressure(int pointerIndex);

            static std::shared_ptr<FakeJni::JString> axisToString(int axis);
            static std::shared_ptr<MotionEvent> obtain(std::shared_ptr<MotionEvent> other);
        };

        class KeyCharacterMap : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/view/KeyCharacterMap")
            static std::shared_ptr<KeyCharacterMap> load(int deviceId);
            int get(int keyCode, int metaState);
        };
    }
    namespace hardware {
        // android/hardware/Sensor. Unity 6 reads the TYPE_* constants with
        // GetStaticIntField after Class.forName() when it enumerates the
        // sensor list it got from SensorManager. The list itself stays empty
        // (no SensorManager stub), so only the constants are needed here.
        class Sensor : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/hardware/Sensor")

            inline static int TYPE_ACCELEROMETER = 1;
            inline static int TYPE_MAGNETIC_FIELD = 2;
            inline static int TYPE_ORIENTATION = 3;
            inline static int TYPE_GYROSCOPE = 4;
            inline static int TYPE_LIGHT = 5;
            inline static int TYPE_PRESSURE = 6;
            inline static int TYPE_TEMPERATURE = 7;
            inline static int TYPE_PROXIMITY = 8;
            inline static int TYPE_GRAVITY = 9;
            inline static int TYPE_LINEAR_ACCELERATION = 10;
            inline static int TYPE_ROTATION_VECTOR = 11;
            inline static int TYPE_RELATIVE_HUMIDITY = 12;
            inline static int TYPE_AMBIENT_TEMPERATURE = 13;
            inline static int TYPE_MAGNETIC_FIELD_UNCALIBRATED = 14;
            inline static int TYPE_GAME_ROTATION_VECTOR = 15;
            inline static int TYPE_GYROSCOPE_UNCALIBRATED = 16;
            inline static int TYPE_SIGNIFICANT_MOTION = 17;
            inline static int TYPE_STEP_DETECTOR = 18;
            inline static int TYPE_STEP_COUNTER = 19;
            inline static int TYPE_GEOMAGNETIC_ROTATION_VECTOR = 20;
            inline static int TYPE_HEART_RATE = 21;
            inline static int TYPE_POSE_6DOF = 28;
            inline static int TYPE_STATIONARY_DETECT = 29;
            inline static int TYPE_MOTION_DETECT = 30;
            inline static int TYPE_HEART_BEAT = 31;
            inline static int TYPE_LOW_LATENCY_OFFBODY_DETECT = 34;
            inline static int TYPE_ACCELEROMETER_UNCALIBRATED = 35;
        };

        namespace display {
            class DisplayManager : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/hardware/display/DisplayManager")

                // android.hardware.display.DisplayManager$DisplayListener.
                // Nested here because the JNI name nests here too.
                class DisplayListener : public virtual FakeJni::JObject {
                public:
                    DEFINE_CLASS_NAME("android/hardware/display/DisplayManager$DisplayListener")
                    virtual void onDisplayAdded(int displayId);
                    virtual void onDisplayChanged(int displayId);
                    virtual void onDisplayRemoved(int displayId);
                };

                std::shared_ptr<jnivm::android::view::Display> getDisplay(int disp);
                std::shared_ptr<jnivm::Array<jnivm::android::view::Display>> getDisplays();

                // Registered listeners are kept (Android holds a strong ref) but
                // never invoked: the panel geometry we report through
                // bd_device_display_* does not change at runtime, so there is no
                // change to report. Revisit if a hotplug/rotation path appears.
                void registerDisplayListener(std::shared_ptr<DisplayListener> listener,
                                             std::shared_ptr<jnivm::android::os::Handler> handler);
                void unregisterDisplayListener(std::shared_ptr<DisplayListener> listener);

            private:
                std::vector<std::shared_ptr<DisplayListener>> listeners;
            };
        }
    }
    namespace media {

        class AudioDeviceInfo : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/AudioDeviceInfo")
            inline static int TYPE_BLUETOOTH_A2DP = 8;
            inline static int TYPE_WIRED_HEADPHONES = 4;
            // getType() lives in binding.cpp via vm->setDefault.
        };

        class AudioManager : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/AudioManager")
            inline static FakeJni::JString PROPERTY_OUTPUT_FRAMES_PER_BUFFER = (FakeJni::JString) "PROPERTY_OUTPUT_FRAMES_PER_BUFFER";
            inline static FakeJni::JString PROPERTY_OUTPUT_SAMPLE_RATE = (FakeJni::JString) "PROPERTY_OUTPUT_SAMPLE_RATE";
            inline static int GET_DEVICES_OUTPUTS = 2;
            inline static int STREAM_MUSIC = 3;
            // SCO_AUDIO_STATE_*: also declared on AudioManager (API 30+); the
            // HFPStatus stub caches its hands-free state against these, so keep
            // the framework values. The loader has no Bluetooth stack, so the
            // state never leaves DISCONNECTED.
            inline static int SCO_AUDIO_STATE_DISCONNECTED = 0;
            inline static int SCO_AUDIO_STATE_CONNECTING = 1;
            inline static int SCO_AUDIO_STATE_CONNECTED = 2;
            inline static int SCO_AUDIO_STATE_ERROR = -1;
            inline static int MODE_NORMAL = 0;
            // isBluetoothA2dpOn / getStreamVolume live in binding.cpp via vm->setDefault.
            std::shared_ptr<FakeJni::JString> getProperty(std::shared_ptr<FakeJni::JString> property);
            std::shared_ptr<jnivm::Array<jnivm::android::media::AudioDeviceInfo>> getDevices(int type);
        };

        class MediaRouterRouteInfo : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/MediaRouter$RouteInfo")
            std::shared_ptr<jnivm::android::view::Display> getPresentationDisplay();
        };

        class MediaRouter : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/MediaRouter")
            inline static int ROUTE_TYPE_LIVE_VIDEO = 2;
            std::shared_ptr<jnivm::android::media::MediaRouterRouteInfo> getSelectedRoute(int type);
        };

        // Factory-stubbed: jnivm builds a typed dummy in defaultVal<jobject>
        // via the registry in android_descriptors.cpp. Method calls fall to
        // type-default returns (int=0, void=no-op, Object=factory recurse).
        class MediaExtractor : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/MediaExtractor")
        };
        class MediaFormat : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/MediaFormat")
        };
        class MediaCodec : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/media/MediaCodec")
        };
    }

    namespace os {

        class Build : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Build");
            inline static FakeJni::JString MANUFACTURER = (FakeJni::JString) "Allwinner";
            inline static FakeJni::JString MODEL = (FakeJni::JString) "h700";
            inline static FakeJni::JString DEVICE = (FakeJni::JString) "R36S";
            inline static FakeJni::JString ID = (FakeJni::JString) "0.01";
            // Unity 6 reads Build.TAGS through JNI (it reports it as
            // SystemInfo / Debug metadata). "release-keys" is what a production
            // Android image reports, letting Unity take the non-debug path.
            inline static FakeJni::JString TAGS = (FakeJni::JString) "release-keys";
        };

        class BuildVersion : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Build$VERSION");
            inline static int SDK_INT = 26;
            inline static FakeJni::JString RELEASE = (FakeJni::JString) "Oreo";
            inline static FakeJni::JString INCREMENTAL = (FakeJni::JString) "Bogodroid";
        };

        class Process : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Process")
            static void setThreadPriority(int i, int j);
        };

        // android.os.LocaleList. Unity 6 asks Configuration.getLocales() for
        // the device's locale list (it uses index 0 for the UI language) and
        // then calls get(0) on it. Both lookups have to land here or the UI
        // language degrades to empty. One entry: the process default locale.
        class LocaleList : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/LocaleList")
            std::shared_ptr<jnivm::java::util::Locale> get(int index);
            int size();
            bool isEmpty();
            static std::shared_ptr<LocaleList> getDefault();
        };

        class Bundle : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Bundle")
            bool containsKey(std::shared_ptr<FakeJni::JString> key);
            // ApplicationInfo.metaData is a Bundle; Unity (2020.3) reads it
            // through the 1-arg overload. Return null when the key is absent,
            // same as real Android.
            std::shared_ptr<FakeJni::JString> getString(std::shared_ptr<FakeJni::JString> key);
            std::shared_ptr<FakeJni::JString> getString(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> def);
            int getInt(std::shared_ptr<FakeJni::JString> key, int def);
            bool getBoolean(std::shared_ptr<FakeJni::JString> key, bool def);
            bool isEmpty();
            int size();
        };

        // Factory-stubbed. See android_descriptors.cpp.
        class ParcelFileDescriptor : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/ParcelFileDescriptor")
        };

        class Handler;

        // Helper function to get a monotonic timestamp in milliseconds
        inline long long uptimeMillis()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        }

        class Message : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Message")
            // Public fields to mimic the Android API
            int what = 0;
            int arg1 = 0;
            int arg2 = 0;
            std::shared_ptr<java::lang::Object> obj = nullptr;
            std::shared_ptr<java::lang::Runnable> callback = nullptr;

            // Internal fields
            long long when = 0; // The absolute time in uptimeMillis when this message should be handled
            Handler* target = nullptr; // The handler that will process this message
            Message() = default;

        private:
            // Private constructor to enforce pooling

            // Next message in the global pool
            static jnivm::android::os::Message* sPool;
            static pthread_mutex_t sPoolMutex;
            Message* next = nullptr;

        public:
            ~Message() = default;
            Message(const jnivm::android::os::Message&) = delete;
            jnivm::android::os::Message& operator=(const jnivm::android::os::Message&) = delete;

            /**
             * Obtains a new Message from the global pool.
             */
            static std::shared_ptr<jnivm::android::os::Message> obtain();
            void sendToTarget();

            /**
             * Returns a Message to the global pool.
             */
            void recycle();
        };

        struct MessageComparer {
            bool operator()(const std::shared_ptr<jnivm::android::os::Message>& lhs, const std::shared_ptr<jnivm::android::os::Message>& rhs) const
            {
                return lhs->when > rhs->when;
            }
        };

        class Looper : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Looper")

            ALooper* nativeLooper; // The underlying native looper
            std::priority_queue<std::shared_ptr<jnivm::android::os::Message>,
                std::vector<std::shared_ptr<jnivm::android::os::Message>>,
                MessageComparer>
                mQueue;

            pthread_mutex_t mQueueMutex;
            std::thread mThread; // Manages the dedicated thread, if any
            bool mQuitting;
            // Private constructor to control instantiation
            Looper();
            ~Looper();
            // No copying
            Looper(const jnivm::android::os::Looper&) = delete;
            jnivm::android::os::Looper& operator=(const jnivm::android::os::Looper&) = delete;
            static void prepare();
            static std::shared_ptr<jnivm::android::os::Looper> myLooper();
            static std::shared_ptr<jnivm::android::os::Looper> getMainLooper();
            void loop();
            void quit();
        };

        class Handler : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Handler")
            class Callback : public virtual FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/os/Handler$Callback")
                virtual ~Callback() = default;
                virtual bool handleMessage(std::shared_ptr<jnivm::android::os::Message> msg);
            };

            std::shared_ptr<jnivm::android::os::Looper> mLooper;
            std::shared_ptr<jnivm::android::os::Handler::Callback> mCallback;

            Handler();
            Handler(std::shared_ptr<jnivm::android::os::Looper> looper);
            Handler(std::shared_ptr<jnivm::android::os::Looper> looper, std::shared_ptr<jnivm::android::os::Handler::Callback> callback);
            bool post(std::shared_ptr<java::lang::Runnable> runnable);
            bool sendMessage(std::shared_ptr<jnivm::android::os::Message> message);
            virtual void handleMessage(std::shared_ptr<jnivm::android::os::Message> message);
            bool postDelayed(std::shared_ptr<java::lang::Runnable> runnable, long delayMillis);
            bool sendMessageAtTime(std::shared_ptr<jnivm::android::os::Message> message, long long uptimeMillis);
            std::shared_ptr<Message> obtainMessage(int what);
        };

        class HandlerThread : public java::lang::Thread {
        private:
            std::shared_ptr<Looper> mLooper;
            std::mutex mMutex;
            std::condition_variable mCv;

        public:
            DEFINE_CLASS_NAME("android/os/HandlerThread", java::lang::Thread)
            HandlerThread(std::shared_ptr<FakeJni::JString> name);
            ~HandlerThread();

            // The main logic for the new thread
            void run() override;

            // The synchronized method to get the Looper
            std::shared_ptr<Looper> getLooper();

            // A way to gracefully shut down the thread
            bool quit();
        };

        class Environment : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/Environment")
            inline static FakeJni::JString MEDIA_MOUNTED = (FakeJni::JString) "MEDIA_MOUNTED";
            static std::shared_ptr<FakeJni::JString> getExternalStorageState();
            static std::shared_ptr<jnivm::java::io::File> getExternalStorageDirectory();
            static bool isExternalStorageManager();
        };

        class StatFs : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/StatFs")

            StatFs(std::shared_ptr<FakeJni::JString> path);
            void restat(std::shared_ptr<FakeJni::JString> path);
            jlong getAvailableBlocksLong();
            jlong getBlockSizeLong();
            jlong getAvailableBytes();
            jlong getFreeBytes();
            jlong getTotalBytes();
            jlong getBlockCountLong();
            jlong getFreeBlocksLong();
        };

        class PowerManager : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/os/PowerManager")
            bool isSustainedPerformanceModeSupported();
        };
    }

    namespace content {
        namespace pm {
            class ActivityInfo : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/pm/ActivityInfo")
                inline static int SCREEN_ORIENTATION_PORTRAIT = 1;
                inline static int SCREEN_ORIENTATION_REVERSE_PORTRAIT = 9;
                inline static int SCREEN_ORIENTATION_REVERSE_LANDSCAPE = 8;
                inline static int SCREEN_ORIENTATION_LANDSCAPE = 0;
                inline static int SCREEN_ORIENTATION_FULL_USER = 13;
                inline static int SCREEN_ORIENTATION_USER_PORTRAIT = 12;
                inline static int SCREEN_ORIENTATION_USER_LANDSCAPE = 11;
                inline static int SCREEN_ORIENTATION_SENSOR = 4;
                inline static int SCREEN_ORIENTATION_UNSPECIFIED = -1;
                // Unity 6 compares ActivityInfo.screenOrientation against these
                // when it decides whether to start the orientation listener and
                // how to map the panel, so they have to be the framework values.
                inline static int SCREEN_ORIENTATION_USER = 2;
                inline static int SCREEN_ORIENTATION_BEHIND = 3;
                inline static int SCREEN_ORIENTATION_NOSENSOR = 5;
                inline static int SCREEN_ORIENTATION_SENSOR_LANDSCAPE = 6;
                inline static int SCREEN_ORIENTATION_SENSOR_PORTRAIT = 7;
                inline static int SCREEN_ORIENTATION_FULL_SENSOR = 10;
                inline static int SCREEN_ORIENTATION_LOCKED = 14;
            };

            class PackageInfo : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/pm/PackageInfo")
                FakeJni::JString versionName = (FakeJni::JString) "0.1";
                // Unity reads versionCode before versionName (and calls
                // versionName.length() right after — see
                // HookStringExtensions). A missing field returns 0 through the
                // placeholder path, which is also a legal version code, so the
                // miss was silent apart from the JNI trace.
                FakeJni::JInt versionCode = 1;
            };

            class ApplicationInfo : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/pm/ApplicationInfo")

                // [BD-DATADIR] absolute path of "<pkg>/data" — Unity reads this
                // field to compute the PlayerPrefs path:
                //   <dataDir>/shared_prefs/<bundle>.v2.playerprefs.xml
                // Populated by Context::getApplicationInfo() from
                // config["paths"]["android_data"].
                std::shared_ptr<FakeJni::JString> dataDir = std::make_shared<FakeJni::JString>("");
                std::shared_ptr<FakeJni::JString> nativeLibraryDir = std::make_shared<FakeJni::JString>("");
                // Unity reflects sourceDir/publicSourceDir when resolving the
                // install-time APK / data-pack zip for MountDataArchive.
                std::shared_ptr<FakeJni::JString> sourceDir = std::make_shared<FakeJni::JString>("");
                std::shared_ptr<FakeJni::JString> publicSourceDir = std::make_shared<FakeJni::JString>("");
                // packageName is what Unity prefixes the prefs name with:
                //   prefsName = applicationInfo.packageName + ".v2.playerprefs"
                // If null/empty, prefs file becomes ".v2.playerprefs.kv" instead
                // of "<pkg>.v2.playerprefs.kv" — same data but wrong filename.
                std::shared_ptr<FakeJni::JString> packageName = std::make_shared<FakeJni::JString>("");
                // PackageItemInfo.metaData. Must stay a *native* type
                // (Bundle, not JObject): jnivm only installs a working field
                // getter for registered native types. A JObject-typed field
                // resolves to a handle-less entry -> "Unknown Field Getter"
                // -> Unity would call getString on a dummy object.
                std::shared_ptr<jnivm::android::os::Bundle> metaData;

                std::shared_ptr<jnivm::Array<FakeJni::JString>> splitPublicSourceDirs = std::make_shared<jnivm::Array<FakeJni::JString>>();
                // Unity reads both through JNI to gate its Android version
                // branches (runtime permissions, scoped storage, window
                // insets). Pinned to the API level the loader claims in
                // Build.VERSION.SDK_INT (26) in bd_make_application_info():
                // everything newer than that expects framework APIs this
                // loader does not implement. [package] minSdkVersion /
                // targetSdkVersion override them.
                int minSdkVersion = 26;
                int targetSdkVersion = 26;
            };

            class PackageManager : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/pm/PackageManager")
                inline static FakeJni::JString FEATURE_AUDIO_LOW_LATENCY = (FakeJni::JString) "FEATURE_AUDIO_LOW_LATENCY";
                inline static int PERMISSION_GRANTED = 0;
                inline static int GET_META_DATA = 0x00000080;
                // getPackageInfo -> registerFactory (PackageInfo).
                bool hasSystemFeature(std::shared_ptr<FakeJni::JString> feature);
                // Returns empty string ("no installer recorded"). Game code
                // typically uses this for analytics/logging only.
                std::shared_ptr<FakeJni::JString> getInstallerPackageName(std::shared_ptr<FakeJni::JString> packageName);
                std::shared_ptr<jnivm::android::content::pm::ApplicationInfo> getApplicationInfo(
                    std::shared_ptr<FakeJni::JString> packageName, int flags);
                std::shared_ptr<jnivm::android::content::pm::PackageInfo> getPackageInfo(
                    std::shared_ptr<FakeJni::JString> packageName, int flags);
            };
        }

        namespace res {
            class AssetManager : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/res/AssetManager")
                std::shared_ptr<jnivm::java::io::InputStream> open(std::shared_ptr<FakeJni::JString> file);
                std::shared_ptr<jnivm::Array<FakeJni::JString>> list(std::shared_ptr<FakeJni::JString> path);
            };

            // android.content.res.Configuration. Unity reads panel size,
            // density and orientation from Resources.getConfiguration() as well
            // as from Display/DisplayMetrics; the three have to agree or Unity
            // picks an inconsistent UI scale. Values come from the same
            // bd_device_display_* source as the other two.
            class Configuration : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/res/Configuration")

                inline static int ORIENTATION_UNDEFINED = 0;
                inline static int ORIENTATION_PORTRAIT = 1;
                inline static int ORIENTATION_LANDSCAPE = 2;
                inline static int ORIENTATION_SQUARE = 3;
                inline static int SCREENLAYOUT_SIZE_MASK = 0x0f;
                inline static int UI_MODE_TYPE_MASK = 0x0f;
                inline static int UI_MODE_TYPE_UNDEFINED = 0;
                inline static int UI_MODE_TYPE_NORMAL = 1;
                inline static int TOUCHSCREEN_NOTOUCH = 1;
                inline static int TOUCHSCREEN_STYLUS = 2;
                inline static int TOUCHSCREEN_FINGER = 3;
                inline static int KEYBOARD_NOKEYS = 1;
                inline static int KEYBOARD_QWERTY = 2;
                inline static int KEYBOARD_12KEY = 3;
                inline static int KEYBOARDHIDDEN_UNDEFINED = 0;
                inline static int KEYBOARDHIDDEN_NO = 1;
                inline static int KEYBOARDHIDDEN_YES = 2;
                inline static int HARDKEYBOARDHIDDEN_UNDEFINED = 0;
                inline static int HARDKEYBOARDHIDDEN_NO = 1;
                inline static int HARDKEYBOARDHIDDEN_YES = 2;
                inline static int NAVIGATION_UNDEFINED = 0;
                inline static int NAVIGATION_NONAV = 1;
                inline static int NAVIGATION_DPAD = 2;
                inline static int NAVIGATION_TRACKBALL = 3;
                inline static int NAVIGATION_WHEEL = 4;
                inline static int NAVIGATIONHIDDEN_UNDEFINED = 0;
                inline static int NAVIGATIONHIDDEN_NO = 1;
                inline static int NAVIGATIONHIDDEN_YES = 2;
                // COLOR_MODE_HDR_* / *_WIDE_COLOR_GAMUT_* live in the high and
                // low nibbles of colorMode; we report neither (0 = UNDEFINED).
                inline static int COLOR_MODE_UNDEFINED = 0;
                inline static int COLOR_MODE_WIDE_COLOR_GAMUT_NO = 1;
                inline static int COLOR_MODE_HDR_NO = 16;
                inline static int COLOR_MODE_DEFAULT = COLOR_MODE_WIDE_COLOR_GAMUT_NO | COLOR_MODE_HDR_NO;

                // Public fields, like the real class: Unity reads them with
                // GetFieldID, not with getters.
                int densityDpi = 0;
                int screenWidthDp = 0;
                int screenHeightDp = 0;
                int smallestScreenWidthDp = 0;
                int orientation = 0;
                int screenLayout = 0;
                int uiMode = 0;
                float fontScale = 1.0f;
                // The rest of android.content.res.Configuration. Unity 6 reads
                // the whole struct through JNI (input capabilities, MCC/MNC,
                // colour mode), so a field that is not registered here comes
                // back as 0 == *_UNDEFINED. make_current() mirrors what
                // thunks/ndk/ndk.cpp answers for the matching AConfiguration
                // queries, so the Java and NDK views of this device agree.
                int mcc = 0;
                int mnc = 0;
                int keyboard = KEYBOARD_NOKEYS;
                int keyboardHidden = KEYBOARDHIDDEN_UNDEFINED;
                int hardKeyboardHidden = HARDKEYBOARDHIDDEN_UNDEFINED;
                int navigation = NAVIGATION_UNDEFINED;
                int navigationHidden = NAVIGATIONHIDDEN_UNDEFINED;
                int touchscreen = TOUCHSCREEN_NOTOUCH;
                int colorMode = COLOR_MODE_UNDEFINED;

                // Fresh instance per call: the real object is mutable and Unity
                // stores/copies it, so a shared singleton would leak state
                // between callers.
                static std::shared_ptr<Configuration> make_current();

                // Configuration.getLocales() — see android/os/LocaleList.
                std::shared_ptr<jnivm::android::os::LocaleList> getLocales();
            };

            class Resources : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/content/res/Resources")
                int getIdentifier(std::shared_ptr<FakeJni::JString> name, std::shared_ptr<FakeJni::JString> defType, std::shared_ptr<FakeJni::JString> defPackage);
                std::shared_ptr<jnivm::android::content::res::Configuration> getConfiguration();
                std::shared_ptr<jnivm::android::util::DisplayMetrics> getDisplayMetrics();
            };
        }

        class SharedPreferences;

        class SharedPreferencesEditor : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/SharedPreferences$Editor")
            // Editor stages changes; apply()/commit() flushes them to its
            // owning SharedPreferences (and onto disk).
            std::weak_ptr<SharedPreferences> owner;
            std::map<std::string, int>          pending_int;
            std::map<std::string, long long>    pending_long;
            std::map<std::string, float>        pending_float;
            std::map<std::string, bool>         pending_bool;
            std::map<std::string, std::string>  pending_string;
            std::set<std::string>               pending_remove;
            bool                                pending_clear = false;

            void apply();
            bool commit();
            std::shared_ptr<SharedPreferencesEditor> putInt(std::shared_ptr<FakeJni::JString> key, int val);
            std::shared_ptr<SharedPreferencesEditor> putLong(std::shared_ptr<FakeJni::JString> key, jlong val);
            std::shared_ptr<SharedPreferencesEditor> putFloat(std::shared_ptr<FakeJni::JString> key, float val);
            std::shared_ptr<SharedPreferencesEditor> putBoolean(std::shared_ptr<FakeJni::JString> key, bool val);
            std::shared_ptr<SharedPreferencesEditor> putString(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> val);
            std::shared_ptr<SharedPreferencesEditor> remove(std::shared_ptr<FakeJni::JString> key);
            std::shared_ptr<SharedPreferencesEditor> clear();
        };

        class SharedPreferences : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/SharedPreferences")
            // [BD-PREFS] in-memory backing; flushed to disk by exit handler
            // (or commit()) to <android_files>/shared_prefs/<name>.kv.
            // Plain text format: one record per line "<type>\t<key>\t<value>"
            // type: i=int  l=long  f=float  b=bool  s=string (escaped).
            std::string                         name;
            std::map<std::string, int>          int_vals;
            std::map<std::string, long long>    long_vals;
            std::map<std::string, float>        float_vals;
            std::map<std::string, bool>         bool_vals;
            std::map<std::string, std::string>  string_vals;
            bool                                dirty = false;
            // commit() rate limit — last successful disk write timestamp
            // (steady_clock seconds since epoch). 0 = never. See commit().
            std::atomic<long long>              last_save_epoch_s{0};

            SharedPreferences() = default;
            void load(const std::string& fromName);
            void save() const;
            // Flush every cached SharedPreferences with dirty=true. Call from
            // exit handler so ALL pending apply() data lands on eMMC.
            static void flush_all();

            bool contains(std::shared_ptr<FakeJni::JString> key);
            int getInt(std::shared_ptr<FakeJni::JString> key, int def);
            jlong getLong(std::shared_ptr<FakeJni::JString> key, jlong def);
            float getFloat(std::shared_ptr<FakeJni::JString> key, float def);
            bool getBoolean(std::shared_ptr<FakeJni::JString> key, bool def);
            std::shared_ptr<FakeJni::JString> getString(std::shared_ptr<FakeJni::JString> key, std::shared_ptr<FakeJni::JString> def);
            // Snapshot of every entry this prefs file holds, as a java.util.Map.
            std::shared_ptr<jnivm::java::util::Map> getAll();
            std::shared_ptr<SharedPreferencesEditor> edit();
        };

        class ContentResolver : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/ContentResolver")
        };

        class Context : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/Context")
            inline static FakeJni::JString LOCATION_SERVICE = (FakeJni::JString) "location";
            inline static FakeJni::JString DISPLAY_SERVICE = (FakeJni::JString) "display";
            inline static FakeJni::JString AUDIO_SERVICE = (FakeJni::JString) "audio";
            inline static FakeJni::JString MEDIA_ROUTER_SERVICE = (FakeJni::JString) "media_router";
            inline static FakeJni::JString POWER_SERVICE = (FakeJni::JString) "power";
            inline static FakeJni::JString INPUT_SERVICE = (FakeJni::JString) "input";
            inline static FakeJni::JString WINDOW_SERVICE = (FakeJni::JString) "window";
            // Declared for completeness: Unity reads the constant and then asks
            // getSystemService() for it. We have no SensorManager / Vibrator
            // stub, so both still resolve to null (getSystemService logs the
            // unknown service) — same behaviour as before, minus the
            // "Unknown Field Getter" noise.
            inline static FakeJni::JString SENSOR_SERVICE = (FakeJni::JString) "sensor";
            inline static FakeJni::JString VIBRATOR_SERVICE = (FakeJni::JString) "vibrator";

            inline static int MODE_PRIVATE = 0;

            std::shared_ptr<FakeJni::JObject> getSystemService(std::shared_ptr<FakeJni::JString> service);
            // Rewired's Android input helper dereferences this during startup;
            // a stub-miss (dummy Object) causes System.NullReferenceException.
            std::shared_ptr<jnivm::android::content::ContentResolver> getContentResolver();
            std::shared_ptr<jnivm::android::content::pm::ApplicationInfo> getApplicationInfo();
            std::shared_ptr<FakeJni::JString> getPackageCodePath();
            std::shared_ptr<FakeJni::JString> getPackageName();
            std::shared_ptr<jnivm::android::content::SharedPreferences> getSharedPreferences(std::shared_ptr<FakeJni::JString> str, int num);
            std::shared_ptr<jnivm::java::io::File> getFilesDir();
            std::shared_ptr<jnivm::java::io::File> getDataDir();
            std::shared_ptr<jnivm::java::io::File> getExternalCacheDir();
            std::shared_ptr<jnivm::java::io::File> getCacheDir();
            std::shared_ptr<jnivm::java::io::File> getExternalFilesDir(std::shared_ptr<FakeJni::JString> path);
            static std::shared_ptr<jnivm::java::io::File> getExternalFilesDirInternal();
            int checkCallingOrSelfPermission(std::shared_ptr<FakeJni::JString> permission);

            // These four are Context methods in the real API (Activity inherits
            // them), so declaring them here lets both Context-typed and
            // Activity-typed callers resolve them.
            std::shared_ptr<jnivm::android::content::res::AssetManager> getAssets();
            std::shared_ptr<jnivm::android::content::pm::PackageManager> getPackageManager();
            std::shared_ptr<jnivm::android::content::res::Resources> getResources();
            // OBB dirs come from [paths] android_obb_dirs. Unity asks for them
            // when the game ships expansion files; returning null (the old
            // STUB-MISS default) makes it treat the install as data-less.
            std::shared_ptr<jnivm::java::io::File> getObbDir();
            std::shared_ptr<jnivm::Array<jnivm::java::io::File>> getObbDirs();
            // getContentResolver / getWindow / getWindowManager ->
            // STUB-MISS path (registerFactory in android_descriptors.cpp).
        };

        class Intent : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/Intent")
            // [package] mainIntentBundle, the same table Bundle reads, so
            // extras look identical whether read through the Intent or the
            // Bundle Unity gets from it.
            std::shared_ptr<jnivm::android::os::Bundle> getExtras();
        };
    }

    namespace app {
        class Activity : public jnivm::android::content::Context {
        public:
            DEFINE_CLASS_NAME("android/app/Activity", jnivm::android::content::Context)
            void runOnUiThread(std::shared_ptr<jnivm::java::lang::Runnable> runnable);
            std::shared_ptr<jnivm::android::content::Intent> getIntent();
            int getRequestedOrientation();
            void setRequestedOrientation(int orientation);
            std::shared_ptr<jnivm::android::content::res::Resources> getResources();
            std::shared_ptr<jnivm::android::view::Window> getWindow();
            std::shared_ptr<jnivm::android::view::WindowManager> getWindowManager();
            std::shared_ptr<jnivm::android::view::View> findViewById(int id);

            // Input
            virtual bool onTouchEvent(std::shared_ptr<android::view::MotionEvent> event) { return false; }
            virtual bool onKeyDown(int keyCode, std::shared_ptr<android::view::KeyEvent> event) { return false; }
            virtual bool onKeyUp(int keyCode, std::shared_ptr<android::view::KeyEvent> event) { return false; }
            virtual bool onGenericMotionEvent(std::shared_ptr<android::view::MotionEvent> event) { return false; }
        };

        class NativeActivity : public jnivm::android::app::Activity {
        public:
            DEFINE_CLASS_NAME("android/app/NativeActivity")
        };

        class DialogInterface : public virtual FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/DialogInterface")
        };

        class DialogInterfaceOnClickListener : public virtual FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/DialogInterface$OnClickListener")
        };

        class DialogInterfaceOnCancelListener : public virtual FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/content/DialogInterface$OnCancelListener")
        };

        class AlertDialog : public jnivm::android::app::DialogInterface {
        public:
            DEFINE_CLASS_NAME("android/app/AlertDialog", jnivm::android::app::DialogInterface)
        };

        
        class AlertDialogBuilder : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/app/AlertDialog$Builder")
            AlertDialogBuilder(std::shared_ptr<jnivm::android::content::Context> context);
            std::shared_ptr<AlertDialogBuilder> setTitle(std::shared_ptr<jnivm::CharSequence> title);
            std::shared_ptr<AlertDialogBuilder> setMessage(std::shared_ptr<jnivm::CharSequence> message);
            std::shared_ptr<AlertDialogBuilder> setPositiveButton(std::shared_ptr<jnivm::CharSequence> text, std::shared_ptr<jnivm::android::app::DialogInterfaceOnClickListener> listener);
            std::shared_ptr<AlertDialogBuilder> setNegativeButton(std::shared_ptr<jnivm::CharSequence> text, std::shared_ptr<jnivm::android::app::DialogInterfaceOnClickListener> listener);
            std::shared_ptr<AlertDialogBuilder> setOnCancelListener(std::shared_ptr<jnivm::android::app::DialogInterfaceOnCancelListener> listener);
            std::shared_ptr<AlertDialogBuilder> setView(std::shared_ptr<jnivm::android::view::View> view);
            std::shared_ptr<jnivm::android::app::AlertDialog> show();
        };
    }

    namespace provider {
        class Settings : public FakeJni::JObject {
        public:
            DEFINE_CLASS_NAME("android/provider/Settings")
            class Secure : public FakeJni::JObject {
            public:
                DEFINE_CLASS_NAME("android/provider/Settings$Secure")
                inline static FakeJni::JString ANDROID_ID = (FakeJni::JString) "android_id"; // B06015BADC0DE00F
                static std::shared_ptr<FakeJni::JString> getString(std::shared_ptr<jnivm::android::content::ContentResolver> resolver, std::shared_ptr<FakeJni::JString> key);
            };
        };
    }
}

}

namespace jnivm::android::view {
class ContextThemeWrapper : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/view/ContextThemeWrapper")
    std::shared_ptr<jnivm::android::content::res::Resources> getResources();
};

class Choreographer : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/view/Choreographer")

    class FrameCallback : public virtual FakeJni::JObject {
    public:
        DEFINE_CLASS_NAME("android/view/Choreographer$FrameCallback")
        virtual ~FrameCallback() = default;

        virtual void doFrame(jlong frameTimeNanos) = 0;
    };
    Choreographer();

private:
    std::shared_ptr<os::HandlerThread> mHandlerThread;
    std::shared_ptr<os::Looper> mLooper;
    std::shared_ptr<os::Handler> mHandler;

    std::vector<std::shared_ptr<FrameCallback>> mCallbacks;
    pthread_mutex_t mCallbacksMutex;

    std::atomic<long long> mLastVSyncTimeNanos;

    void watchdogLoop();

    void dispatchFrameCallbacks(bool isRealVSync);

public:
    static std::shared_ptr<Choreographer> getInstance();
    ~Choreographer();

    void postFrameCallback(std::shared_ptr<FrameCallback> callback);
    void signalVSync(); // The public method for eglSwapBuffers
};
}

namespace jnivm::android::hardware::input {
class InputManager : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/hardware/input/InputManager")
    class InputDeviceListener : public virtual FakeJni::JObject {
    public:
        DEFINE_CLASS_NAME("android/hardware/input/InputManager$InputDeviceListener")
    };
    std::shared_ptr<jnivm::Array<int>> getInputDeviceIds();
    std::shared_ptr<jnivm::android::view::InputDevice> getInputDevice(int device);
    void registerInputDeviceListener(std::shared_ptr<jnivm::android::hardware::input::InputManager::InputDeviceListener> listener, std::shared_ptr<jnivm::android::os::Handler> handler);
};
}
#endif
