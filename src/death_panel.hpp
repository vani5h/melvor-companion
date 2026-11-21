#pragma once
#include <windows.h>
#include <span>
#include <cstdint>

namespace companion {
// The pixels are top-down BGRA, as returned by a 32-bit Windows DIB.
bool has_death_banner(std::span<const std::uint32_t> pixels, int width, int height);
bool window_has_death_banner(HWND window, DWORD expected_process);
}
