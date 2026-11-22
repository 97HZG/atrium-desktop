// The length of a track in NetEase Cloud Music's play queue. The player tells Windows the track but
// no timeline; it keeps its queue, each track with its length, in its own files
// (%LOCALAPPDATA%\NetEase\CloudMusic\webdata\file\playingList), read again only when that changes.
// Media worker thread.
#pragma once

#include "shell/media_timeline.h"
#include "shell/netease_memory.h"

#include <cstdint>
#include <string>

namespace atrium {

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
