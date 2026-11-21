#pragma once
#include "game_activity.hpp"
#include <windows.h>
#include <chrono>

namespace companion {
// Opens the Detailed Summary window (replacing one that is already open). It is non-modal:
// the tray keeps working while it is open. `new_save` is false when Melvor has not saved since
// the game was hidden, so no change can be shown yet.
void show_summary(const ProgressChange& change, std::chrono::seconds hidden_for, bool new_save, HICON icon);

// Gives an open summary window its dialog keyboard handling (Tab, Enter, Esc). Call from the
// message loop before dispatching; returns true when the message was handled.
bool summary_message(MSG& message);

// The open summary window, if any (for tests).
HWND summary_window();
}
