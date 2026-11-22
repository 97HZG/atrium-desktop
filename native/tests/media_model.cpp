#include "shell/media_players.h"
#include "shell/media_timeline.h"

#include <iostream>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckPlayers()
{
    Require(PlayerOf(L"cloudmusic.exe") == MediaPlayer::Netease, "NetEase Cloud Music by its exe");
    Require(PlayerOf(L"QQMusic.exe") == MediaPlayer::QqMusic && PlayerOf(L"Tencent.QQMusic") == MediaPlayer::QqMusic, "QQ Music, old and new names");
    Require(PlayerOf(L"Tencent.QQ") == MediaPlayer::Other && PlayerOf(L"QQMusic.exe.bak") == MediaPlayer::Other, "not QQ Music");
    Require(PlayerOf(L"Spotify.exe") == MediaPlayer::Spotify && PlayerOf(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify") == MediaPlayer::Spotify,
            "Spotify, desktop and Store");
    Require(PlayerOf(L"AppleInc.AppleMusicWin_nzyj5cx40ttqa!App") == MediaPlayer::AppleMusic, "Apple Music");
    Require(PlayerOf(L"msedge.exe") == MediaPlayer::Other, "a browser is another player");
    std::wstring artist = L"\u738B\u83F2 \u2014 \u53EA\u7231\u964C\u751F\u4EBA", album;
    SplitAppleArtist(artist, album);
    Require(artist == L"\u738B\u83F2" && album == L"\u53EA\u7231\u964C\u751F\u4EBA", "Apple Music's artist and album apart");
    std::wstring named = L"Artist \u2014 Alias", other = L"Other Album";
    SplitAppleArtist(named, other);
    Require(named == L"Artist \u2014 Alias", "a dash in a name with another album stays");
}

void CheckEstimate()
{
    TimelineEstimate e;
    MediaTimeline t;
    e.Update(L"a", true, 180'000'000, 0, t);
    Require(!t.Seekable(), "a track under way when first seen has no timeline");
    e.Update(L"b", true, 200'000'000, 1000, t);
    Require(t.Seekable() && t.endUs == 200'000'000, "a track seen to start gets the length");
    Require(t.PositionUs(11'000) == 10'000'000, "its position counts from the change");
    MediaTimeline paused;
    e.Update(L"b", false, 200'000'000, 21'000, paused);
    Require(!paused.playing && paused.PositionUs(90'000) == 20'000'000, "a pause holds the position");
    MediaTimeline resumed;
    e.Update(L"b", true, 200'000'000, 50'000, resumed);
    Require(resumed.PositionUs(55'000) == 25'000'000, "playing again goes on from there");
    MediaTimeline again;
    e.Update(L"b", true, 200'000'000, 300'000, again);
    Require(again.PositionUs(300'000) == 0, "past the end on the same track: played again from 0");
    MediaTimeline unknown;
    e.Update(L"c", true, 0, 400'000, unknown);
    Require(!unknown.Seekable(), "no length: no timeline");
}

void CheckTimeline()
{
    MediaTimeline tl;
    tl.startUs = 10'000'000;
    tl.endUs = 70'000'000;
    tl.anchorPosUs = 20'000'000;
    tl.anchorMs = 1000.0;
    tl.rate = 1.0;
    tl.playing = false;

    Require(tl.Seekable(), "positive range is seekable");
    Require(tl.PositionUs(5000.0) == 20'000'000, "paused timeline stays at anchor");

    tl.playing = true;
    Require(tl.PositionUs(2500.0) == 21'500'000, "1x playback advances linearly");

    tl.rate = 1.5;
    Require(tl.PositionUs(3000.0) == 23'000'000, "1.5x playback scales elapsed time");

    Require(tl.PositionUs(100'000.0) == 70'000'000, "timeline clamps to endUs");
    Require(tl.PositionUs(-100'000.0) == 10'000'000, "timeline clamps to startUs");

    MediaTimeline live;
    live.startUs = 0;
    live.endUs = 0;
    live.anchorPosUs = 5'000'000;
    live.anchorMs = 1000.0;
    live.playing = true;
    Require(!live.Seekable(), "endUs <= startUs is live and not seekable");
    Require(live.PositionUs(2000.0) == 6'000'000, "live timeline still advances");
    Require(live.PositionUs(-10'000.0) == 0, "live timeline clamps at startUs");
}

void CheckAppFilter()
{
    Require(MediaAppAllowed(L"Spotify.exe", L"all", {L"Spotify.exe"}), "all mode ignores list");
    Require(MediaAppAllowed(L"Spotify.exe", L"", {L"Spotify.exe"}), "empty mode defaults to all");

    const std::vector<std::wstring> list{
        L"spotify", L"cloudmusic.exe", L"Microsoft.ZuneMusic_", L"SpotifyAB.SpotifyMusic", L"C:\\Apps\\\u00c9diteur\\Player.exe",
    };

    Require(MediaAppAllowed(L"C:\\Users\\a\\AppData\\Spotify.exe", L"allow", list), "stem matches full path exe");
    Require(MediaAppAllowed(L"Spotify.EXE", L"allow", list), "case-insensitive exe matches stem");
    Require(MediaAppAllowed(L"D:\\Music\\CloudMusic.exe", L"allow", list), "exe name matches different install path");
    Require(MediaAppAllowed(L"cloudmusic", L"allow", list), "appId without .exe matches .exe entry");
    Require(MediaAppAllowed(L"microsoft.zunemusic_8wekyb3d8bbwe!App", L"allow", list), "trailing underscore package prefix matches");
    Require(MediaAppAllowed(L"SpotifyAB.SpotifyMusic_zpdnekdrzrea0!Spotify", L"allow", list), "package name prefix matches before underscore");
    Require(MediaAppAllowed(L"c:\\apps\\\u00e9diteur\\player.exe", L"allow", list), "non-ASCII path matches case-insensitively");

    Require(!MediaAppAllowed(L"D:\\Other\\Player.exe", L"allow", list), "full path entry does not match different path");
    Require(!MediaAppAllowed(L"spot.exe", L"allow", list), "partial name does not false-match");
    Require(!MediaAppAllowed(L"Microsoft.Zune_8wekyb3d8bbwe!App", L"allow", list), "shorter package name does not false-match");
    Require(!MediaAppAllowed(L"", L"allow", list), "empty appId is rejected in allow mode");

    Require(!MediaAppAllowed(L"Spotify.exe", L"block", list), "block mode rejects listed app");
    Require(MediaAppAllowed(L"vlc.exe", L"block", list), "block mode allows unlisted app");
    Require(MediaAppAllowed(L"", L"block", list), "empty appId is not blocked");
}

void CheckPickSession()
{
    Require(PickMediaSession({}, true, L"all", {}, L"") == -1, "empty sessions returns -1");

    const std::vector<MediaSessionSummary> sessions{
        {L"closed", L"spotify.exe", L"Song A", MediaPlaybackState::Closed, true, 900.0},
        {L"empty_paused", L"spotify.exe", L"", MediaPlaybackState::Paused, false, 800.0},
        {L"paused_old", L"vlc.exe", L"Movie", MediaPlaybackState::Paused, false, 100.0},
        {L"paused_new", L"foobar2000.exe", L"Track", MediaPlaybackState::Paused, false, 500.0},
        {L"playing_recent", L"chrome.exe", L"Video", MediaPlaybackState::Playing, false, 700.0},
        {L"playing_current", L"spotify.exe", L"Song B", MediaPlaybackState::Playing, true, 400.0},
    };

    Require(PickMediaSession(sessions, true, L"all", {}, L"") == 5, "Playing + isSystemCurrent wins");
    Require(PickMediaSession(sessions, true, L"block", {L"spotify"}, L"") == 4, "blocked current falls back to recent Playing");
    Require(PickMediaSession(sessions, true, L"allow", {L"vlc", L"foobar2000"}, L"") == 3, "no Playing falls back to most recent Paused");
    Require(PickMediaSession(sessions, false, L"allow", {L"vlc", L"foobar2000"}, L"") == -1, "showPaused=false skips Paused");
    Require(PickMediaSession(sessions, true, L"all", {}, L"paused_old") == 2, "pinnedId selects eligible session");
    Require(PickMediaSession(sessions, false, L"all", {}, L"paused_old") == 5, "ineligible pinnedId falls back to normal priority");
    Require(PickMediaSession(sessions, true, L"all", {}, L"empty_paused") == 5, "empty-title paused session is ignored");

    const std::vector<MediaSessionSummary> emptyTitlePlaying{
        {L"radio", L"foobar2000.exe", L"", MediaPlaybackState::Playing, false, 10.0},
    };
    Require(PickMediaSession(emptyTitlePlaying, true, L"all", {}, L"") == 0, "Playing session without title is still eligible");
}

} // namespace

int main()
{
    CheckTimeline();
    CheckEstimate();
    CheckPlayers();
    CheckAppFilter();
    CheckPickSession();
    if (g_failures > 0)
        return 1;
    std::cout << "media model: all checks passed\n";
    return 0;
}
