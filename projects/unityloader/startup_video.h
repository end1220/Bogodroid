#pragma once
#include <cstdint>
#include <memory>

// A bounded, silent video decoder. Owned by the startup presentation thread;
// keeps the displayed RGBA frame, the latest due frame, and one pending frame.
class StartupVideo {
public:
    StartupVideo();
    ~StartupVideo();
    bool open(const char* path);
    bool update(uint32_t elapsed_ms);
    int width() const;
    int height() const;
    const uint8_t* pixels() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
