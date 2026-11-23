// Games, for everything that must keep out of their way: Windows' Game Bar's list of games (the
// GameConfigStore, by program path), programs under the game stores' library folders, the folders and
// programs the user marked for Stage Manager (stageExcluded paths: the settings app's "find games"), and
// any app holding the screen exclusively (Direct3D full screen). A game in front filling its screen (full
// screen, borderless or maximized) brings nothing up over it from the pointer, and Stage Manager never
// puts its windows away. Cheap enough per call: program paths are cached by process.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace atrium {

// The stageExcluded entries that are paths (folders end in a backslash): games the user marked.
void SetUserGames(const std::vector<std::wstring>& entries);
bool IsGameWindow(HWND window);
// The window in front on `monitor` is a game filling it (or the screen is held exclusively).
bool GameInFront(HMONITOR monitor);

} // namespace atrium
