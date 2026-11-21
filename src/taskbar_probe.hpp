#pragma once
#include <memory>
#include <optional>

namespace companion {
// Where Win11 really draws the app buttons. MSTaskSwWClass can report a stale edge (~250 px
// short at 175%). UIA calls go into Explorer and can be slow, so they run on a worker thread.
class TaskbarProbe {
public:
    TaskbarProbe();
    ~TaskbarProbe();
    TaskbarProbe(const TaskbarProbe&) = delete;
    TaskbarProbe& operator=(const TaskbarProbe&) = delete;

    // non-blocking
    void request();
    // right edge (horizontal) or bottom edge (vertical) of the last button;
    // nullopt when unknown, e.g. Windows 10
    struct Extent { int right; int bottom; };
    std::optional<Extent> app_buttons_end() const;

    struct State;
private:
    std::shared_ptr<State> state_; // shared with the thread
};
}
