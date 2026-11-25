// While a Windows screenshot tool is on screen, widget glass must not sample the
// live desktop: that sample includes the desktop icons, and they show through the
// widget in the picture. Wallpaper has no icons.
#pragma once

namespace atrium {

// True while Snipping Tool, Screen Clipping Host or Screen Sketch has a visible window.
bool ScreenshotInProgress();

// Enumerates top-level windows. Call from the widget UI thread a few times a second.
// Returns true when the active state flipped.
bool PollScreenshotWatch();

} // namespace atrium
