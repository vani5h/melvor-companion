#pragma once
#include <windows.h>
#include <audiopolicy.h>
#include <wrl/client.h>
#include <string>
#include <vector>

namespace companion {
class HiddenAudio {
public:
    HiddenAudio() noexcept;
    ~HiddenAudio();
    HiddenAudio(const HiddenAudio&) = delete;
    HiddenAudio& operator=(const HiddenAudio&) = delete;
    void mute(DWORD pid) noexcept;
    void restore() noexcept;
private:
    struct Session {
        std::wstring id;
        Microsoft::WRL::ComPtr<ISimpleAudioVolume> volume;
    };
    HRESULT apartment_;
    std::vector<Session> changed_;
};
}
