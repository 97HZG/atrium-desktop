// Pure backdrop source selection, retry backoff, and black-capture fallback policy.
#pragma once

#include <cstdint>
#include <optional>

namespace atrium {

class CapturePolicy {
public:
    enum class InitialSource { Wallpaper, Duplication, MonitorCapture };
    enum class BlackAction { RestartCapture, SwitchToMonitorCapture, FallbackToWallpaper };

    static constexpr uint64_t kRetryBackoffMs = 30000;
    static constexpr uint64_t kBlackDetectMs = 1500;
    static constexpr uint64_t kBlackWindowMs = 30000;
    static constexpr float kBlackLumaThreshold = 0.01f;
    static constexpr int kMaxBlackRestarts = 2;

    void BeginSession() { blackSince_.reset(); }

    InitialSource ChooseInitialSource(bool remote, uint64_t nowMs) const
    {
        if (remote || nowMs < wallpaperUntil_)
            return InitialSource::Wallpaper;
        return duplicationFailed_ ? InitialSource::MonitorCapture : InitialSource::Duplication;
    }

    void OnDuplicationUnavailable(uint64_t nowMs)
    {
        duplicationFailed_ = true;
        duplicationRetryAt_ = nowMs + kRetryBackoffMs;
    }

    bool ShouldProbeDuplication(bool needScreen, bool remote, bool captureKeepsOwnOut, unsigned captureGeneration, uint64_t nowMs)
    {
        if (!needScreen || remote || !duplicationFailed_ || captureKeepsOwnOut || nowMs < duplicationRetryAt_)
            return false;
        if (blackGeneration_ && *blackGeneration_ == captureGeneration)
            return false;
        duplicationRetryAt_ = nowMs + kRetryBackoffMs;
        return true;
    }

    void OnDuplicationRecovered()
    {
        duplicationFailed_ = false;
        blackGeneration_.reset();
    }

    bool ShouldExitBlackoutWallpaper(bool blackOut, uint64_t nowMs) const { return blackOut && nowMs >= wallpaperUntil_; }

    // Keep the GDI ScreenLitAbove() sample gated behind the 1500 ms dark-backdrop timer.
    bool PollBlackBackdrop(bool eligible, float brightestLuma, uint64_t nowMs)
    {
        if (!eligible || brightestLuma > kBlackLumaThreshold) {
            blackSince_.reset();
            if (blackRestartMs_ && nowMs >= *blackRestartMs_ && nowMs - *blackRestartMs_ > kBlackWindowMs) {
                blackRestarts_ = 0;
                blackRestartMs_.reset();
            }
            return false;
        }
        if (!blackSince_) {
            blackSince_ = nowMs;
            return false;
        }
        if (nowMs < *blackSince_ || nowMs - *blackSince_ <= kBlackDetectMs)
            return false;
        blackSince_.reset();
        return true;
    }

    BlackAction OnBlackOverLitScreen(unsigned captureGeneration, uint64_t nowMs)
    {
        blackRestartMs_ = nowMs;
        if (++blackRestarts_ < kMaxBlackRestarts)
            return BlackAction::RestartCapture;
        blackRestarts_ = 0;
        blackRestartMs_.reset();
        if (duplicationFailed_) {
            wallpaperUntil_ = nowMs + kRetryBackoffMs;
            return BlackAction::FallbackToWallpaper;
        }
        duplicationFailed_ = true;
        duplicationRetryAt_ = nowMs + kRetryBackoffMs;
        blackGeneration_ = captureGeneration;
        return BlackAction::SwitchToMonitorCapture;
    }

private:
    bool duplicationFailed_ = false;
    uint64_t duplicationRetryAt_ = 0;
    std::optional<unsigned> blackGeneration_;
    uint64_t wallpaperUntil_ = 0;
    int blackRestarts_ = 0;
    std::optional<uint64_t> blackRestartMs_;
    std::optional<uint64_t> blackSince_;
};

} // namespace atrium
