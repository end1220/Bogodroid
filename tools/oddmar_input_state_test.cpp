#include "projects/unityloader/plugins/oddmar_input/oddmar_input_state.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static constexpr int a = 0, b = 1, x = 2, guide = 5, start = 6;
static constexpr uint32_t menu_mask = (1u << start) | (1u << guide);

int main()
{
    OddmarInputState input{menu_mask};
    for (int button = 0; button < 4; ++button) {
        input.note_button(button, true, 10);
        input.begin_frame(11);
        assert(input.buttons() == (1u << button));
        assert(input.pressed(button));
        input.note_button(button, false, 12);
        assert(input.buttons() == (1u << button)); // Stable within the frame.
        input.begin_frame(13);
        assert(input.buttons() == 0);
        assert(!input.pressed(button));
    }
    for (int tap = 0; tap < 12; ++tap) {
        const uint32_t now = 100 + tap * 10;
        assert(input.note_button(x, true, now));
        assert(!input.note_button(x, true, now + 1)); // No hardware-repeat edge.
        input.note_button(x, false, now + 2);
        input.begin_frame(now + 3);
        assert(input.pressed(x)); // Quick taps between frames survive.
        assert(input.pressed(x)); // Multiple consumers see the same edge.
        input.begin_frame(now + 4);
        assert(!input.pressed(x)); // Never replay it in a later frame.
    }
    input.note_button(x, true, 250);
    input.begin_frame(251);
    assert(input.pressed(x));
    input.begin_frame(252);
    assert(!input.pressed(x)); // Holding X does not synthesize attacks.
    input.note_button(x, false, 253);

    OddmarInputState gameplay{menu_mask};
    for (int button : {a, b, x}) {
        for (int tap = 0; tap < 3; ++tap) {
            assert(gameplay.note_button(button, true, 260));
            gameplay.begin_frame(261);
            for (int action : {a, b, x})
                assert(gameplay.pressed(action) == (action == button));
            gameplay.begin_frame(262);
            assert(!gameplay.pressed(button)); // Held A/B/X never repeats.
            assert(gameplay.buttons() == (1u << button));
            gameplay.note_button(button, false, 263);
            gameplay.begin_frame(264);
            assert(!gameplay.pressed(button)); // Release is not a press.
        }
    }

    assert(!input.allow_pause_transition(true)); // ABXY cannot open pause.
    input.note_button(start, true, 300);
    input.begin_frame(301);
    assert(input.ui_command_available());
    assert(input.allow_pause_transition(true));
    assert(!input.allow_pause_transition(false)); // Same frame open -> close.
    assert(!input.ui_command_available());
    input.note_button(start, true, 302);
    input.begin_frame(303);
    assert(!input.allow_pause_transition(false)); // Same held input, next frame.
    input.note_button(b, true, 304);
    input.begin_frame(305);
    assert(!input.ui_command_available());
    assert(!input.allow_pause_transition(false)); // Opening key still held.
    input.note_button(start, false, 306);
    input.note_button(b, false, 307);
    input.note_button(b, true, 310);
    input.begin_frame(311);
    assert(input.ui_command_available()); // Fresh B cancel remains usable.
    assert(input.allow_pause_transition(false));
    assert(!input.allow_pause_transition(false));
    input.note_button(b, false, 312);
    input.note_button(start, true, 320);
    input.begin_frame(321);
    assert(input.allow_pause_transition(true)); // Release then Start re-arms.
    input.note_button(start, false, 322);
    input.note_button(a, true, 330);
    input.begin_frame(331);
    assert(input.ui_command_available());
    assert(input.allow_pause_transition(false)); // A may confirm Resume.

    OddmarInputState quit{menu_mask};
    quit.note_button(guide, true, 400);
    quit.note_button(guide, false, 401);
    quit.begin_frame(402);
    assert(quit.consume_menu());
    assert(!quit.consume_menu());
    assert(!quit.ui_command_available()); // Opening event cannot cancel popup.
    quit.begin_frame(403);
    assert(!quit.ui_command_available()); // Release cannot cancel it either.
    quit.note_button(a, true, 410);
    quit.begin_frame(411);
    assert(quit.ui_command_available());

    OddmarInputState moving{menu_mask};
    moving.note_button(14, true, 430); // Already holding D-pad while playing.
    moving.begin_frame(431);
    moving.note_button(start, true, 440);
    moving.begin_frame(441);
    assert(moving.allow_pause_transition(true));
    moving.note_button(start, false, 442);
    moving.note_button(a, true, 450);
    moving.begin_frame(451);
    assert(moving.ui_command_available());
    assert(moving.allow_pause_transition(false)); // No need to release D-pad.

    OddmarInputState expired{menu_mask};
    expired.note_button(start, true, 500);
    expired.begin_frame(801);
    assert(!expired.consume_menu());
    assert(!expired.allow_pause_transition(true));
    OddmarInputState wrapped{menu_mask};
    wrapped.note_button(start, true, UINT32_MAX - 10);
    wrapped.begin_frame(9);
    assert(wrapped.allow_pause_transition(true));
    std::puts("PASS: frame snapshots, repeated X edges, single menu transition, fresh UI commands");
}
