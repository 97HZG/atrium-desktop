#include "extras_host.h"

#include "bar.h"
#include "log.h"
#include "posted.h"
#include "settings.h"

namespace atrium {

namespace {

// The workers' news, on ExtrasHost's own window.
constexpr UINT WM_MEDIA_STATE = WM_APP + 1; // lParam: MediaSnapshot*
constexpr UINT WM_STACKS_READY = WM_APP + 2;
constexpr wchar_t kWindowClass[] = L"AtriumExtras";

// nowPlayingMotion as the bars' reach.
float Intensity(const std::wstring& motion)
{
    if (motion == L"off")
        return 0.f;
    if (motion == L"subtle")
        return 0.55f;
    return motion == L"vivid" ? 1.45f : 1.f;
}

// A click on an entry whose popup is open closes it; the click that just closed it (the popup lost
// the focus to the bar) does not open it again.
template<class Popup> bool JustClosed(const Popup& popup) { return GetTickCount64() - popup.LastClosed() < 400; }

} // namespace

HWND ExtrasHost::CreateHost(HINSTANCE instance, ExtrasHost* self)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc); // a second host (never made) would find it registered
    const HWND h = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (h)
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    return h;
}

LRESULT CALLBACK ExtrasHost::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<ExtrasHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_MEDIA_STATE) {
        const auto snapshot = TakePosted<MediaSnapshot>(l); // always taken back; a stale one is dropped
        if (self && self->media_)
            self->OnMedia(*snapshot);
        return 0;
    }
    if (self && m == WM_STACKS_READY) {
        self->stacks_.Refresh();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

ExtrasHost::ExtrasHost(HINSTANCE instance, AppCatalog& catalog, ForEachBar forBars)
    : forBars_(std::move(forBars)),
      host_(CreateHost(instance, this)),
      feed_(std::make_shared<MusicFeed>()),
      launchpad_(instance, catalog),
      card_(instance),
      stackModel_(host_, WM_STACKS_READY),
      stacks_(instance, stackModel_)
{
    card_.SetFeed(feed_);
    card_.onCommand = [this](MediaCommand command, int64_t positionUs) {
        if (media_)
            media_->Command(command, positionUs);
    };
    stacks_.onManageFolders = [this] {
        if (onOpenSettings)
            onOpenSettings(L"stacks");
    };
}

ExtrasHost::~ExtrasHost()
{
    Shutdown();
    if (host_) {
        SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        DestroyWindow(host_);
    }
}

void ExtrasHost::Apply(const Settings& s, float radius)
{
    feed_->intensity.store(Intensity(s.nowPlayingMotion));
    scrollVolume_ = s.nowPlayingScrollVolume;
    musicOn_ = s.showNowPlaying;
    if (musicOn_ && !media_) {
        const std::shared_ptr<MusicFeed> feed = feed_;
        media_ = std::make_unique<MediaSessionWatcher>(host_, WM_MEDIA_STATE, [feed](float peak, bool playing) {
            feed->peak.store(peak);
            feed->playing.store(playing);
        });
    } else if (!musicOn_ && media_) {
        media_.reset();
        OnMedia(MediaSnapshot{}); // the entry and its card go with the setting
    }
    if (media_)
        media_->SetFilter(s.nowPlayingShowPaused, s.nowPlayingAppMode, s.nowPlayingApps);
    stacksOn_ = s.showFolderStacks;
    stacks_.SetOptions(s.folderStackLayout != L"column", size_t(s.folderStackMax));
    stackModel_.SetFolders(stacksOn_ ? s.folderStacks : std::vector<std::wstring>{}, 96); // read once at a size every scale draws from
    if (!stacksOn_)
        stacks_.Close();
    if (!s.showLaunchpad)
        launchpad_.Close(false);
    card_.SetRadius(radius);
    stacks_.SetRadius(radius);
}

void ExtrasHost::ConfigureBar(Bar& bar)
{
    bar.SetMusic(feed_, musicShown_, MusicTip());
    bar.onExtraClick = [this, &bar](const std::wstring& key, RECT entry) { Click(bar, key, entry); };
    bar.onMusicWheel = [this](int notches) {
        if (media_ && scrollVolume_)
            media_->AdjustVolume(2 * notches);
    };
}

void ExtrasHost::Click(Bar& bar, const std::wstring& key, RECT entry)
{
    if (key == L"#launchpad") {
        if (launchpad_.Visible()) {
            launchpad_.Close(false);
            return;
        }
        if (JustClosed(launchpad_))
            return;
        MONITORINFO mi{sizeof(mi)};
        RECT barRect = bar.BarRect();
        GetMonitorInfoW(MonitorFromRect(&barRect, MONITOR_DEFAULTTONEAREST), &mi);
        launchpad_.Open(mi.rcMonitor, bar.PreviousForeground());
    } else if (key == L"#music") {
        if (card_.Visible() || JustClosed(card_)) {
            card_.Close();
            return;
        }
        card_.Open(entry, bar.BarRect());
    } else if (key == L"#stacks") {
        if (stacks_.Visible() || JustClosed(stacks_)) {
            stacks_.Close();
            return;
        }
        stacks_.Open(entry, bar.BarRect());
    } else {
        return;
    }
    if (onPanelsChanged)
        onPanelsChanged();
}

void ExtrasHost::OnMedia(const MediaSnapshot& snapshot)
{
    const bool playing = snapshot.active && snapshot.state == MediaPlaybackState::Playing;
    if (!playing)
        feed_->peak.store(0.f);
    feed_->playing.store(playing);
    const bool shown = musicOn_ && snapshot.active;
    const bool changed = shown != musicShown_;
    musicShown_ = shown;
    last_ = snapshot;
    // Every bar learns whether the entry is there (a layout) and wakes for the bars' motion.
    const std::wstring tip = MusicTip();
    forBars_([&](Bar& bar) { bar.SetMusic(feed_, shown, tip); });
    if (changed)
        Log(L"now playing: entry %s", shown ? L"shown" : L"gone");
    card_.Update(snapshot);
}

std::wstring ExtrasHost::MusicTip() const
{
    if (last_.title.empty())
        return last_.appName;
    return last_.artist.empty() ? last_.title : last_.title + L" \u00b7 " + last_.artist;
}

void ExtrasHost::OnCatalogChanged() { launchpad_.Refresh(); }

void ExtrasHost::ClosePopups()
{
    launchpad_.Close(false);
    card_.Close();
    stacks_.Close();
}

bool ExtrasHost::AnyPopupVisible() const { return launchpad_.Visible() || card_.Visible() || stacks_.Visible(); }

void ExtrasHost::Shutdown()
{
    ClosePopups();
    media_.reset();
}

} // namespace atrium
