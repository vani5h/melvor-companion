#include "audio.hpp"
#include <mmdeviceapi.h>
#include <algorithm>
#include <memory>

namespace companion {
using Microsoft::WRL::ComPtr;
HiddenAudio::HiddenAudio() noexcept : apartment_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
HiddenAudio::~HiddenAudio() {
    restore();
    changed_.clear();
    if (SUCCEEDED(apartment_)) CoUninitialize();
}
void HiddenAudio::restore() noexcept {
    for (auto it = changed_.begin(); it != changed_.end();) {
        if (SUCCEEDED(it->volume->SetMute(FALSE, nullptr))) it = changed_.erase(it);
        else ++it;
    }
}
void HiddenAudio::mute(DWORD pid) noexcept {
    if (FAILED(apartment_) && apartment_ != RPC_E_CHANGED_MODE) return;
    try {
        ComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&devices)))) return;
        ComPtr<IMMDeviceCollection> endpoints;
        if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &endpoints))) return;
        UINT count{};
        if (FAILED(endpoints->GetCount(&count))) return;
        for (UINT e = 0; e < count; ++e) {
            ComPtr<IMMDevice> endpoint;
            ComPtr<IAudioSessionManager2> manager;
            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(endpoints->Item(e, &endpoint)) ||
                FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
                FAILED(manager->GetSessionEnumerator(&sessions))) continue;
            int n{};
            if (FAILED(sessions->GetCount(&n))) continue;
            for (int i = 0; i < n; ++i) {
                ComPtr<IAudioSessionControl> session;
                ComPtr<IAudioSessionControl2> control;
                ComPtr<ISimpleAudioVolume> volume;
                DWORD owner{};
                if (FAILED(sessions->GetSession(i, &session)) || FAILED(session.As(&control)) ||
                    FAILED(control->GetProcessId(&owner)) || owner != pid || FAILED(session.As(&volume))) continue;
                LPWSTR raw{};
                if (FAILED(control->GetSessionInstanceIdentifier(&raw))) continue;
                std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> id(raw, CoTaskMemFree);
                if (std::any_of(changed_.begin(), changed_.end(), [&](const auto& item) { return item.id == raw; })) continue;
                BOOL muted{};
                if (FAILED(volume->GetMute(&muted)) || muted) continue;
                // Allocate tracking before changing system state.
                changed_.push_back({raw, volume});
                if (FAILED(volume->SetMute(TRUE, nullptr))) changed_.pop_back();
            }
        }
    } catch (...) {
        OutputDebugStringW(L"Melvor Companion: could not enumerate game audio sessions.\n");
    }
}
}

