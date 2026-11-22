// The length of a track in NetEase Cloud Music's play queue. The player tells Windows the track but
// no timeline; it keeps its queue, each track with its length, in its own files
// (%LOCALAPPDATA%\NetEase\CloudMusic\webdata\file\playingList), read again only when that changes.
// Media worker thread.
#pragma once

#include <cstdint>
#include <string>

namespace atrium {

bool IsNeteasePlayer(const std::wstring& appId); // cloudmusic.exe

// The queued track named `title` (by `artist`, when the queue has several of that name); 0: not found.
int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist);

} // namespace atrium
