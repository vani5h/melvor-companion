# Building from source

Melvor Companion is plain C++20 and the Win32 API: no runtime, no frameworks.

## Setup

Install Visual Studio Build Tools with **Desktop development with C++**, including the x64 MSVC tools, a Windows SDK, and C++ CMake tools for Windows ([installation guide](https://learn.microsoft.com/en-us/cpp/build/vscpp-step-0-installation?view=msvc-170)).

Open an **x64 Native Tools Command Prompt** and check that the tools are found:

```bat
where cl
where rc
cmake --version
```

## Build

From the project directory:

```bat
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

The app is `build\Release\MelvorCompanion.exe`. A running copy locks that file, so exit it from its tray menu before rebuilding (the linker reports `LNK1104` otherwise).

## Package a release

```bat
cpack --config build/CPackConfig.cmake -C Release -B build/packages
```

This creates `MelvorCompanion-<version>-windows-x64.zip` and its SHA256 file: the executable, user guide, screenshots, and licenses.

## Version and update check

The version lives in `project(MelvorCompanion VERSION ...)` in `CMakeLists.txt`. It feeds the executable's version resource, the About box, the single-instance hand-off, and the update check. Releases are tagged like `v0.1.1`. The update check asks the GitHub repository set by `MELVOR_COMPANION_UPDATE_REPO`; a fork can point it elsewhere, or set it empty to hide the menu item:

```bat
cmake -S . -B build -DMELVOR_COMPANION_UPDATE_REPO=owner/repo
```

## Logs

Tray actions, taskbar widget placement decisions, and errors are timestamped in `%LOCALAPPDATA%\Melvor Companion\companion.log`. The file starts over when it would grow past 128 KiB.

## Settings

Settings live under `HKEY_CURRENT_USER\Software\Melvor Companion`, and the startup entry is the `Melvor Companion` value under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. These names stay fixed so settings carry over between versions.
