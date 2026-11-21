#pragma once
#include "game_activity.hpp"
#include <windows.h>
#include <chrono>

namespace companion {
// Non-modal. new_save is false when Melvor hasn't saved since hiding.
void show_summary(const ProgressChange& change, std::chrono::seconds hidden_for, bool new_save, HICON icon);

// dialog keys (Tab, Enter, Esc); call from the message loop
bool summary_message(MSG& message);

HWND summary_window();
}
