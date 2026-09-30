#pragma once

#include <cstdint>

// Access is serialized by the SDL bridge. Game queries use one render-frame
// snapshot; a tap between frames still contributes its press edge.
class OddmarInputState {
public:
    explicit OddmarInputState(uint32_t menu_mask) : menu_mask_(menu_mask) {}

    bool note_button(int button, bool down, uint32_t now)
    {
        if (button < 0 || button >= 32) return false;
        const uint32_t bit = uint32_t{1} << button;
        if (!down) {
            held_ &= ~bit;
            transition_held_ &= ~bit;
            return false;
        }
        if (held_ & bit) return false;
        held_ |= bit;
        pending_pressed_ |= bit;
        ++serial_;
        press_at_ = now;
        if (bit & menu_mask_) {
            menu_serial_ = serial_;
            menu_at_ = now;
        }
        return true;
    }

    void begin_frame(uint32_t now)
    {
        frame_pressed_ = pending_pressed_;
        frame_buttons_ = held_ | pending_pressed_;
        pending_pressed_ = 0;
        frame_serial_ = serial_;
        frame_menu_serial_ = menu_serial_;
        frame_menu_at_ = menu_at_;
        frame_press_at_ = press_at_;
        frame_at_ = now;
    }

    uint32_t buttons() const { return frame_buttons_; }
    bool pressed(int button) const
    {
        return button >= 0 && button < 32 &&
               (frame_pressed_ & (uint32_t{1} << button)) != 0;
    }
    uint64_t frame_serial() const { return frame_serial_; }

    bool ui_command_available() const
    {
        return (!transition_serial_ && !consumed_menu_serial_) ||
               (frame_serial_ != transition_serial_ &&
                frame_serial_ != consumed_menu_serial_ &&
                (held_ & transition_held_) == 0);
    }

    bool consume_menu()
    {
        if (!frame_menu_serial_ || frame_menu_serial_ == consumed_menu_serial_ ||
            uint32_t(frame_at_ - frame_menu_at_) > 300) return false;
        consumed_menu_serial_ = frame_menu_serial_;
        return true;
    }

    bool allow_pause_transition(bool playing)
    {
        if (!frame_serial_ || frame_serial_ == transition_serial_ ||
            uint32_t(frame_at_ - frame_press_at_) > 300) return false;
        if (playing) {
            if (!consume_menu()) return false;
        } else {
            // Opening/closing must not reuse a held key, even through a
            // second UI handler. Release it before issuing a new command.
            if (held_ & transition_held_) return false;
            consumed_menu_serial_ = frame_menu_serial_;
        }
        transition_serial_ = frame_serial_;
        transition_held_ = held_ & frame_pressed_;
        return true;
    }

private:
    uint32_t menu_mask_;
    uint32_t held_ = 0;
    uint32_t pending_pressed_ = 0;
    uint32_t frame_pressed_ = 0;
    uint32_t frame_buttons_ = 0;
    uint32_t transition_held_ = 0;
    uint32_t press_at_ = 0, frame_press_at_ = 0;
    uint32_t menu_at_ = 0, frame_menu_at_ = 0, frame_at_ = 0;
    uint64_t serial_ = 0, frame_serial_ = 0;
    uint64_t menu_serial_ = 0, frame_menu_serial_ = 0;
    uint64_t consumed_menu_serial_ = 0, transition_serial_ = 0;
};
