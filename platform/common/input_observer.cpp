#include "input_observer.h"
#include <mutex>
#include <vector>

namespace {
std::mutex g_observer_mutex;
std::vector<BogoInputObserver> g_observers;
}

int bd_register_input_observer(const BogoInputObserver* observer)
{
    if (!observer || (!observer->button && !observer->axis &&
                      !observer->frame_begin))
        return 0;
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    g_observers.push_back(*observer);
    return 1;
}

void bd_input_observer_note_button(int button, int down)
{
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    for (const auto& observer : g_observers) {
        if (observer.button)
            observer.button(button, down, observer.userdata);
    }
}

void bd_input_observer_note_axis(int axis, int value)
{
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    for (const auto& observer : g_observers) {
        if (observer.axis)
            observer.axis(axis, value, observer.userdata);
    }
}

void bd_input_observer_begin_frame(void)
{
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    for (const auto& observer : g_observers) {
        if (observer.frame_begin)
            observer.frame_begin(observer.userdata);
    }
}
