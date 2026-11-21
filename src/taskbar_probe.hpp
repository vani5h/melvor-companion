#pragma once
#include <memory>
#include <optional>

namespace companion {
// Measures where Windows 11 actually draws the taskbar's app buttons. The legacy MSTaskSwWClass
// window that used to hold them can report a stale rectangle (seen at 175% scaling, where it ended
// about 250 px before the real last button). UI Automation reports what is really on screen.
//
// UI Automation calls go into Explorer and can be slow, and the taskbar widget is itself a child
// of Explorer's taskbar, so the calls run on a background thread; the UI thread only reads the
// last result and never waits.
class TaskbarProbe {
public:
    TaskbarProbe();
    ~TaskbarProbe();
    TaskbarProbe(const TaskbarProbe&) = delete;
    TaskbarProbe& operator=(const TaskbarProbe&) = delete;

    // Asks for a fresh measurement soon. Non-blocking; starts the thread on first use.
    void request();
    // Where the app buttons end, in screen pixels: the right edge of the last one (horizontal
    // taskbars) and the bottom edge of the last one (vertical taskbars). nullopt when unknown
    // (no measurement yet, or a taskbar without these elements, such as Windows 10's).
    struct Extent { int right; int bottom; };
    std::optional<Extent> app_buttons_end() const;

    struct State; // defined in taskbar_probe.cpp; shared with the background thread
private:
    std::shared_ptr<State> state_; // shared with the thread, so it outlives the tray if needed
};
}
