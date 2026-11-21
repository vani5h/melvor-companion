#include "taskbar_probe.hpp"
#include <windows.h>
// The project defines WIN32_LEAN_AND_MEAN, which leaves COM out of windows.h; UI Automation
// headers need it.
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>

namespace companion {
using Microsoft::WRL::ComPtr; // releases the COM object when it goes out of scope (RAII for COM)

struct TaskbarProbe::State {
    std::mutex lock;
    std::optional<TaskbarProbe::Extent> end;
    bool started{};
    bool stop{};
    std::chrono::steady_clock::time_point last_request{};
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    ~State() { if (wake) CloseHandle(wake); }
};

namespace {
// Rightmost app button on the primary taskbar, as drawn.
std::optional<TaskbarProbe::Extent> measure(IUIAutomation* automation) {
    HWND bar = FindWindowW(L"Shell_TrayWnd", nullptr);
    RECT bounds{};
    if (!bar || !GetWindowRect(bar, &bounds)) return std::nullopt;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(bar, &root)) || !root) return std::nullopt;
    VARIANT name;
    VariantInit(&name);
    name.vt = VT_BSTR;
    name.bstrVal = SysAllocString(L"Taskbar.TaskListButtonAutomationPeer");
    ComPtr<IUIAutomationCondition> condition;
    HRESULT created = name.bstrVal ? automation->CreatePropertyCondition(UIA_ClassNamePropertyId, name, &condition) : E_OUTOFMEMORY;
    VariantClear(&name);
    if (FAILED(created)) return std::nullopt;
    ComPtr<IUIAutomationElementArray> buttons;
    int count{};
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &buttons)) || !buttons
        || FAILED(buttons->get_Length(&count)))
        return std::nullopt;
    std::optional<TaskbarProbe::Extent> end;
    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> button;
        RECT rect{};
        if (FAILED(buttons->GetElement(i, &button)) || !button || FAILED(button->get_CurrentBoundingRectangle(&rect)))
            continue;
        // Only buttons drawn on this taskbar (not overflow flyouts or other monitors).
        if (rect.right <= rect.left || rect.left < bounds.left || rect.right > bounds.right
            || rect.bottom <= bounds.top || rect.top >= bounds.bottom)
            continue;
        if (!end) end = TaskbarProbe::Extent{rect.right, rect.bottom};
        end->right = std::max<int>(end->right, rect.right);
        end->bottom = std::max<int>(end->bottom, rect.bottom);
    }
    return end;
}

void run(std::shared_ptr<TaskbarProbe::State> state) {
    // UI Automation needs COM on this thread; the multithreaded apartment suits a worker.
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
    {
        ComPtr<IUIAutomation> automation;
        if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))) {
            using namespace std::chrono;
            for (;;) {
                {
                    std::unique_lock guard(state->lock);
                    if (state->stop) break;
                    // Idle (no wake-ups) once the tray has not asked for 30 seconds, e.g. tray mode.
                    if (steady_clock::now() - state->last_request > seconds(30)) {
                        guard.unlock(); // never hold the lock while waiting
                        WaitForSingleObject(state->wake, INFINITE);
                        guard.lock();
                        if (state->stop) break;
                    }
                }
                auto measured = measure(automation.Get());
                {
                    std::lock_guard guard(state->lock);
                    state->end = measured;
                }
                // Measure again when asked (placement inputs changed) or every 10 s, which
                // catches apps opening and closing.
                WaitForSingleObject(state->wake, 10000);
            }
        }
    } // releases COM objects before CoUninitialize
    CoUninitialize();
}
}

TaskbarProbe::TaskbarProbe() : state_(std::make_shared<State>()) {}

TaskbarProbe::~TaskbarProbe() {
    {
        std::lock_guard guard(state_->lock);
        state_->stop = true;
    }
    if (state_->wake) SetEvent(state_->wake);
    // The thread is detached and holds its own reference to the state, so the tray never waits
    // for a slow call into Explorer while exiting.
}

void TaskbarProbe::request() {
    bool start = false;
    {
        std::lock_guard guard(state_->lock);
        state_->last_request = std::chrono::steady_clock::now();
        if (!state_->started && state_->wake) start = state_->started = true;
    }
    if (start) std::thread(run, state_).detach();
    else if (state_->wake) SetEvent(state_->wake);
}

std::optional<TaskbarProbe::Extent> TaskbarProbe::app_buttons_end() const {
    std::lock_guard guard(state_->lock);
    return state_->end;
}
}
