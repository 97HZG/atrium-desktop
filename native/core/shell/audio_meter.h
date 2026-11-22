// WASAPI peak meter and per-app volume adjustment for the Now Playing indicator.
#pragma once

#include <memory>
#include <string>

namespace atrium {

// Samples normalized audio peak levels (0.0..1.0) on the caller's MTA thread,
// matching a target playback app by AUMID or executable name with master fallback.
class AudioPeakMeter {
public:
    AudioPeakMeter();
    ~AudioPeakMeter();
    AudioPeakMeter(const AudioPeakMeter&) = delete;
    AudioPeakMeter& operator=(const AudioPeakMeter&) = delete;
    AudioPeakMeter(AudioPeakMeter&&) noexcept;
    AudioPeakMeter& operator=(AudioPeakMeter&&) noexcept;

    float Sample(const std::wstring& appIdOrExe);
    bool AdjustAppVolume(const std::wstring& appIdOrExe, int deltaPercent);
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace atrium
