#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*BogoInputButtonCallback)(int button, int down, void* userdata);
typedef void (*BogoInputAxisCallback)(int axis, int value, void* userdata);
typedef void (*BogoInputFrameCallback)(void* userdata);

typedef struct BogoInputObserver {
    BogoInputButtonCallback button;
    BogoInputAxisCallback axis;
    BogoInputFrameCallback frame_begin;
    void* userdata;
} BogoInputObserver;

int bd_register_input_observer(const BogoInputObserver* observer);
void bd_input_observer_note_button(int button, int down);
void bd_input_observer_note_axis(int axis, int value);
void bd_input_observer_begin_frame(void);

#ifdef __cplusplus
}
#endif
