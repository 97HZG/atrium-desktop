#include "shell/netease_memory.h"

#include "log.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace atrium {

namespace {

// Versions whose places are known (cloudmusic.dll's PE time stamp and image size): the one Daozai
// verified, then those found here. The first is also where a new version's search starts.
struct KnownVersion {
    uint32_t stamp, size;
    uintptr_t position, length;
};
constexpr KnownVersion kKnown[] = {
    {0x6ad712e1, 41054208, 0x21a74d8, 0x2214778},
    {0x6ae94a6a, 41058304, 0x21a84d8, 0x2215798},
};
constexpr uintptr_t kKnownPosition = kKnown[0].position, kKnownLength = kKnown[0].length;
constexpr double kRetryMs = 5000;      // nothing found: try again after
constexpr double kProbeMs = 900;       // between two readings that must differ by the time between them
constexpr double kTolerance = 0.3;     // s
constexpr size_t kChunk = 1 << 20;     // bytes read at a time
constexpr size_t kFewCandidates = 64;  // fewer: read one by one rather than scanned again
constexpr int kMaxFailures = 4;        // full scans in vain: the process is left alone

bool ValidLength(double v) { return std::isfinite(v) && v > 0 && v < 86400; }

} // namespace

bool NeteaseMemory::Attach()
{
    Detach();
    winrt::handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap)
        return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        if (_wcsicmp(pe.szExeFile, L"cloudmusic.exe") != 0)
            continue;
        winrt::handle process{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID)};
        if (!process)
            continue;
        std::array<HMODULE, 1024> modules{};
        DWORD needed = 0;
        if (!K32EnumProcessModules(process.get(), modules.data(), DWORD(sizeof(modules)), &needed))
            continue;
        for (size_t i = 0; i < std::min<size_t>(needed / sizeof(HMODULE), modules.size()); ++i) {
            wchar_t name[MAX_PATH] = {};
            if (!K32GetModuleBaseNameW(process.get(), modules[i], name, MAX_PATH) || _wcsicmp(name, L"cloudmusic.dll") != 0)
                continue;
            process_ = std::move(process);
            base_ = reinterpret_cast<uintptr_t>(modules[i]);
            // The PE header: the version, and the writable sections where the values live.
            std::array<uint8_t, 4096> header{};
            SIZE_T read = 0;
            if (!ReadProcessMemory(process_.get(), modules[i], header.data(), header.size(), &read) || read != header.size()) {
                Detach();
                return false;
            }
            uint32_t peAt = 0;
            std::memcpy(&peAt, header.data() + 60, 4);
            if (peAt > 4096 - 24 - 240) {
                Detach();
                return false;
            }
            uint16_t count = 0, optional = 0;
            std::memcpy(&stamp_, header.data() + peAt + 8, 4);
            std::memcpy(&imageSize_, header.data() + peAt + 24 + 56, 4);
            std::memcpy(&count, header.data() + peAt + 6, 2);
            std::memcpy(&optional, header.data() + peAt + 20, 2);
            for (size_t s = 0; s < count; ++s) {
                const size_t at = peAt + 24 + optional + s * 40;
                if (at + 40 > header.size())
                    break;
                uint32_t size = 0, rva = 0, flags = 0;
                std::memcpy(&size, header.data() + at + 8, 4);
                std::memcpy(&rva, header.data() + at + 12, 4);
                std::memcpy(&flags, header.data() + at + 36, 4);
                if (flags & IMAGE_SCN_MEM_WRITE)
                    sections_.emplace_back(rva, size);
            }
            Log(L"netease: cloudmusic.dll %08x/%u in %lu", stamp_, imageSize_, pe.th32ProcessID);
            return true;
        }
    }
    return false;
}

void NeteaseMemory::Detach()
{
    process_.close();
    base_ = 0;
    sections_.clear();
    lengthRva_ = positionRva_ = 0;
    trusted_ = false;
    candidates_.clear();
    probeMs_ = 0;
    failures_ = 0;
}

bool NeteaseMemory::ReadDouble(uintptr_t rva, double& value) const
{
    SIZE_T read = 0;
    return ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(base_ + rva), &value, sizeof(value), &read) && read == sizeof(value);
}

NeteaseMemory::Doubles NeteaseMemory::Scan(double lo, double hi) const
{
    Doubles found;
    std::vector<uint8_t> bytes(kChunk);
    for (const auto& [rva, size] : sections_)
        for (uintptr_t off = 0; off < size; off += kChunk) {
            const size_t n = std::min<size_t>(kChunk, size - off) & ~size_t(7);
            SIZE_T read = 0;
            if (!ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(base_ + rva + off), bytes.data(), n, &read))
                continue;
            for (size_t i = 0; i + 8 <= read; i += 8) {
                double v = 0;
                std::memcpy(&v, bytes.data() + i, 8);
                if (std::isfinite(v) && v >= lo && v <= hi)
                    found.emplace_back(rva + off + i, v);
            }
        }
    return found;
}

// The length's place: a known version's, else the one double equal to the queue's length (nearest
// the known place if several); the position's first guess moved with it.
bool NeteaseMemory::Locate(double lengthSec)
{
    for (const KnownVersion& v : kKnown)
        if (stamp_ == v.stamp && imageSize_ == v.size) {
            lengthRva_ = v.length;
            positionRva_ = v.position;
            trusted_ = true;
            return true;
        }
    if (!ValidLength(lengthSec))
        return false;
    const Doubles equal = Scan(lengthSec - 0.0005, lengthSec + 0.0005);
    if (equal.empty())
        return false;
    auto distance = [](uintptr_t rva) { return rva > kKnownLength ? rva - kKnownLength : kKnownLength - rva; };
    lengthRva_ = std::ranges::min(equal, {}, [&](const auto& e) { return distance(e.first); }).first;
    const uintptr_t guess = kKnownPosition + lengthRva_ - kKnownLength;
    double v = 0;
    candidates_.clear();
    if (ReadDouble(guess, v))
        candidates_.emplace_back(guess, v);
    probeMs_ = 0;
    Log(L"netease: the length at %llx (%zu equal), the position first tried at %llx", uint64_t(lengthRva_), equal.size(), uint64_t(guess));
    return true;
}

// The position's place: of the candidates, the one that advanced by the time between two readings
// while playing; with no candidate left, every double up to the length is one.
void NeteaseMemory::Verify(bool playing, double nowMs, double length)
{
    if (!playing) {
        probeMs_ = 0; // paused: start over from the next reading while playing
        return;
    }
    if (probeMs_ == 0) {
        if (candidates_.empty())
            candidates_ = Scan(0, length);
        else
            for (auto& [rva, value] : candidates_)
                ReadDouble(rva, value);
        probeMs_ = nowMs;
        return;
    }
    if (nowMs - probeMs_ < kProbeMs)
        return;
    const double elapsed = (nowMs - probeMs_) / 1000;
    Doubles moved;
    if (candidates_.size() <= kFewCandidates) {
        for (const auto& [rva, before] : candidates_)
            if (double now = 0; ReadDouble(rva, now) && std::abs(now - before - elapsed) < kTolerance)
                moved.emplace_back(rva, now);
    } else {
        const Doubles again = Scan(0, length);
        auto a = candidates_.begin();
        for (const auto& [rva, now] : again) {
            while (a != candidates_.end() && a->first < rva)
                ++a;
            if (a != candidates_.end() && a->first == rva && std::abs(now - a->second - elapsed) < kTolerance)
                moved.emplace_back(rva, now);
        }
    }
    probeMs_ = nowMs;
    if (moved.size() == 1) {
        positionRva_ = moved.front().first;
        trusted_ = true;
        candidates_.clear();
        Log(L"netease: the position at %llx", uint64_t(positionRva_));
    } else if (moved.empty()) {
        candidates_.clear(); // the guess (or every candidate) failed: a full scan next
        if (++failures_ >= kMaxFailures) {
            Log(L"netease: no position found; the track's time is estimated");
            retryMs_ = nowMs + 600000; // ten minutes: perhaps the player reloads
        }
    } else {
        candidates_ = std::move(moved); // several still: the next reading decides
    }
}

bool NeteaseMemory::Read(double lengthSec, bool playing, double nowMs, double& position, double& length)
{
    if (nowMs < retryMs_)
        return false;
    DWORD code = 0;
    if (process_ && (!GetExitCodeProcess(process_.get(), &code) || code != STILL_ACTIVE))
        Detach();
    if ((!process_ && !Attach()) || (!lengthRva_ && !Locate(lengthSec))) {
        retryMs_ = nowMs + kRetryMs;
        return false;
    }
    double len = 0, pos = 0;
    if (!ReadDouble(lengthRva_, len) || !ValidLength(len)) {
        lengthRva_ = positionRva_ = 0; // moved (a reload): found again
        trusted_ = false;
        return false;
    }
    if (lengthSec > 0 && std::abs(len - lengthSec) > 0.5)
        return false; // the player has not caught up with the track yet
    if (!trusted_)
        Verify(playing, nowMs, len);
    if (!trusted_ || !ReadDouble(positionRva_, pos) || !std::isfinite(pos) || pos < 0 || pos > len + 1)
        return false;
    position = std::min(pos, len);
    length = len;
    return true;
}

} // namespace atrium
