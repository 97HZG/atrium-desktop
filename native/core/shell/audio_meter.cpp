#include "shell/audio_meter.h"

#include "win32_path.h"

#include <windows.h>
#include <appmodel.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace atrium {

namespace {

constexpr uint64_t kSessionRefreshIntervalMs = 1000;

struct ScopedHandle {
    HANDLE handle = nullptr;
    explicit ScopedHandle(HANDLE h) : handle(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ~ScopedHandle()
    {
        if (handle)
            CloseHandle(handle);
    }
};

std::wstring NormalizeExeStem(const std::wstring& value)
{
    std::wstring folded = FoldCase(value);
    const size_t slash = folded.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        folded = folded.substr(slash + 1);
    if (folded.size() > 4 && folded.compare(folded.size() - 4, 4, L".exe") == 0)
        folded.resize(folded.size() - 4);
    return folded;
}

std::wstring ProcessAumid(HANDLE process)
{
    UINT32 len = 0;
    if (GetApplicationUserModelId(process, &len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return {};
    std::wstring aumid(len, L'\0');
    if (GetApplicationUserModelId(process, &len, aumid.data()) != ERROR_SUCCESS)
        return {};
    if (!aumid.empty() && aumid.back() == L'\0')
        aumid.pop_back();
    return aumid;
}

bool SessionMatchesApp(IAudioSessionControl2* control, const std::wstring& targetFolded, const std::wstring& targetStem)
{
    if (!control || targetFolded.empty() || control->IsSystemSoundsSession() == S_OK)
        return false;
    DWORD pid = 0;
    if (FAILED(control->GetProcessId(&pid)) || pid == 0)
        return false;
    const ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.handle)
        return false;
    const std::wstring aumid = FoldCase(ProcessAumid(process.handle));
    if (!aumid.empty() && aumid == targetFolded)
        return true;
    const std::wstring path = FoldCase(ProcessImagePath(process.handle));
    if (path.empty())
        return false;
    if (path == targetFolded)
        return true;
    return !targetStem.empty() && NormalizeExeStem(path) == targetStem;
}

} // namespace

struct AudioPeakMeter::Impl {
    winrt::com_ptr<IMMDevice> device;
    winrt::com_ptr<IAudioSessionManager2> sessionManager;
    winrt::com_ptr<IAudioMeterInformation> masterMeter;
    std::vector<winrt::com_ptr<IAudioMeterInformation>> sessionMeters;
    std::wstring cachedAppId;
    uint64_t lastRefreshMs = 0;

    bool EnsureDevice();
    bool RefreshSessions(const std::wstring& appIdOrExe);
};

bool AudioPeakMeter::Impl::EnsureDevice()
{
    if (device && sessionManager && masterMeter)
        return true;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(enumerator.put()))) || !enumerator)
        return false;

    winrt::com_ptr<IMMDevice> dev;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, dev.put()))) {
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())))
            return false;
    }
    if (!dev)
        return false;

    winrt::com_ptr<IAudioSessionManager2> manager;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_INPROC_SERVER, nullptr, manager.put_void())) || !manager)
        return false;

    winrt::com_ptr<IAudioMeterInformation> meter;
    if (FAILED(dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_INPROC_SERVER, nullptr, meter.put_void())) || !meter)
        return false;

    device = std::move(dev);
    sessionManager = std::move(manager);
    masterMeter = std::move(meter);
    return true;
}

bool AudioPeakMeter::Impl::RefreshSessions(const std::wstring& appIdOrExe)
{
    sessionMeters.clear();
    cachedAppId = appIdOrExe;
    lastRefreshMs = GetTickCount64();

    if (appIdOrExe.empty() || !sessionManager)
        return true;

    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    const HRESULT enumHr = sessionManager->GetSessionEnumerator(sessions.put());
    if (enumHr == AUDCLNT_E_DEVICE_INVALIDATED)
        return false;
    if (FAILED(enumHr) || !sessions)
        return true;

    const std::wstring targetFolded = FoldCase(appIdOrExe);
    const std::wstring targetStem = NormalizeExeStem(appIdOrExe);
    int count = 0;
    sessions->GetCount(&count);
    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, control.put())) || !control)
            continue;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired)
            continue;
        auto control2 = control.try_as<IAudioSessionControl2>();
        if (!SessionMatchesApp(control2.get(), targetFolded, targetStem))
            continue;
        if (auto meter = control.try_as<IAudioMeterInformation>())
            sessionMeters.push_back(std::move(meter));
    }
    return true;
}

AudioPeakMeter::AudioPeakMeter() : impl_(std::make_unique<Impl>()) {}
AudioPeakMeter::~AudioPeakMeter() = default;
AudioPeakMeter::AudioPeakMeter(AudioPeakMeter&&) noexcept = default;
AudioPeakMeter& AudioPeakMeter::operator=(AudioPeakMeter&&) noexcept = default;

void AudioPeakMeter::Reset()
{
    if (!impl_)
        return;
    impl_->device = nullptr;
    impl_->sessionManager = nullptr;
    impl_->masterMeter = nullptr;
    impl_->sessionMeters.clear();
    impl_->cachedAppId.clear();
    impl_->lastRefreshMs = 0;
}

float AudioPeakMeter::Sample(const std::wstring& appIdOrExe)
{
    if (!impl_ || !impl_->EnsureDevice())
        return 0.0f;

    const uint64_t now = GetTickCount64();
    if (appIdOrExe != impl_->cachedAppId || impl_->lastRefreshMs == 0 || (now - impl_->lastRefreshMs) >= kSessionRefreshIntervalMs) {
        if (!impl_->RefreshSessions(appIdOrExe)) {
            Reset();
            return 0.0f;
        }
    }

    if (!impl_->sessionMeters.empty()) {
        float maxPeak = 0.0f;
        for (const auto& meter : impl_->sessionMeters) {
            float peak = 0.0f;
            const HRESULT hr = meter->GetPeakValue(&peak);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                Reset();
                return 0.0f;
            }
            if (SUCCEEDED(hr))
                maxPeak = std::max(maxPeak, peak);
        }
        return std::clamp(maxPeak, 0.0f, 1.0f);
    }

    float masterPeak = 0.0f;
    const HRESULT hr = impl_->masterMeter->GetPeakValue(&masterPeak);
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Reset();
        return 0.0f;
    }
    return SUCCEEDED(hr) ? std::clamp(masterPeak, 0.0f, 1.0f) : 0.0f;
}

bool AudioPeakMeter::AdjustAppVolume(const std::wstring& appIdOrExe, int deltaPercent)
{
    if (appIdOrExe.empty() || !impl_ || !impl_->EnsureDevice())
        return false;

    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    const HRESULT enumHr = impl_->sessionManager->GetSessionEnumerator(sessions.put());
    if (enumHr == AUDCLNT_E_DEVICE_INVALIDATED) {
        Reset();
        return false;
    }
    if (FAILED(enumHr) || !sessions)
        return false;

    const std::wstring targetFolded = FoldCase(appIdOrExe);
    const std::wstring targetStem = NormalizeExeStem(appIdOrExe);
    int count = 0;
    sessions->GetCount(&count);
    bool adjusted = false;

    for (int i = 0; i < count; ++i) {
        winrt::com_ptr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, control.put())) || !control)
            continue;
        AudioSessionState state = AudioSessionStateExpired;
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired)
            continue;
        auto control2 = control.try_as<IAudioSessionControl2>();
        if (!SessionMatchesApp(control2.get(), targetFolded, targetStem))
            continue;
        auto volume = control.try_as<ISimpleAudioVolume>();
        if (!volume)
            continue;
        float level = 0.0f;
        if (FAILED(volume->GetMasterVolume(&level)))
            continue;
        const long nextPct = std::clamp(std::lround(level * 100.0f) + long(deltaPercent), 0L, 100L);
        const float nextLevel = float(nextPct) / 100.0f;
        const HRESULT setHr = volume->SetMasterVolume(nextLevel, nullptr);
        if (setHr == AUDCLNT_E_DEVICE_INVALIDATED) {
            Reset();
            return false;
        }
        if (SUCCEEDED(setHr)) {
            if (nextPct > 0)
                volume->SetMute(FALSE, nullptr);
            adjusted = true;
        }
    }
    return adjusted;
}

} // namespace atrium
