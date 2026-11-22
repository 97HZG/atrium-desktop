// The length of a track in NetEase Cloud Music's play queue. The player tells Windows the track but
// no timeline; it keeps its queue, each track with its length, in its own files
// (%LOCALAPPDATA%\NetEase\CloudMusic\webdata\file\playingList), read again only when that changes.
// Media worker thread.
#pragma once

#include "shell/media_timeline.h"
#include "shell/netease_memory.h"

#include <cstdint>
#include <string>
#include <vector>

namespace atrium {

struct NeteaseQueuedTrack {
    std::wstring title, artists; // artists: their names joined by "/", as the player tells Windows
    int64_t lengthUs = 0;
};

class NeteaseQueueCache {
public:
    bool NeedsRead(uint64_t writeTime, uint64_t sizeBytes) const { return writeTime != writeTime_ || sizeBytes != sizeBytes_; }
    bool Update(const std::wstring& text, uint64_t writeTime, uint64_t sizeBytes);
    int64_t FindLengthUs(const std::wstring& title, const std::wstring& artist) const;

private:
    uint64_t writeTime_ = 0;
    uint64_t sizeBytes_ = 0;
    std::vector<NeteaseQueuedTrack> tracks_;
};

// The queued track named `title` (by `artist`, when the queue has several of that name); 0: not found.
int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist);

// The timeline the player does not report: its own position, read from its memory twice a second,
// else estimated from when the track was seen to start (TimelineEstimate).
class NeteaseTimeline {
public:
    void Fill(const std::wstring& title, const std::wstring& artist, bool playing, double nowMs, MediaTimeline& t);

private:
    NeteaseMemory memory_;
    TimelineEstimate estimate_;
    std::wstring track_;
    int64_t lengthUs_ = 0;
    double lookedUpMs_ = -1e9, readMs_ = -1e9;
    bool playing_ = false, fromMemory_ = false;
    MediaTimeline last_; // from the memory: kept while the readings agree with it, so the card is not told again
};

} // namespace atrium
