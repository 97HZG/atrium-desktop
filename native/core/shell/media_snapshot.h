// What the media session watcher (shell/media_session.h) hands the UI thread: the session the
// now-playing entry follows, as the card shows it. One immutable copy per change.
#pragma once

#include "icon_image.h"
#include "shell/media_timeline.h"

#include <cstdint>
#include <memory>
#include <string>

namespace atrium {

struct MediaSnapshot {
    bool active = false; // a session to show: the entry is on the bar
    std::wstring sessionId, appId, appName, title, artist, album;
    MediaPlaybackState state = MediaPlaybackState::Closed;
    MediaTimeline timeline;
    bool canPlayPause = false, canNext = false, canPrevious = false, canSeek = false;
    std::shared_ptr<const IconImage> artwork; // the album art, square; null: none
    uint64_t artworkVersion = 0;              // changes with the picture
    int sessionIndex = 0, sessionCount = 0;   // among the sessions shown (MediaCommand::NextSession)
};

// What the card asks of the session. Seek carries the position (microseconds from the track's start).
enum class MediaCommand { PlayPause, Next, Previous, Seek, NextSession, ActivateApp };

} // namespace atrium
