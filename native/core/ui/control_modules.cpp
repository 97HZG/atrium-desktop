// The control centre's optional modules (controlModules): under the connections, two to a row (one
// alone in its row takes it whole), each a round switch with its name and state as macOS's tiles. Stage
// Manager's goes to the core; the system's (shell/quick_toggles) are read when the centre opens and
// flipped on the slow worker, shown flipped at once.
#include "ui/control_center.h"

#include "localization.h"
#include "log.h"
#include "shell/quick_toggles.h"
#include "ui/control_layout.h"

#include <shellapi.h>

#include <algorithm>

namespace atrium {

namespace {

using control::kGap;
using control::kModuleH;
using control::kPad;
using control::kTopH;
using control::kW;

struct ModuleSpec {
    const wchar_t* id;
    const wchar_t* glyph;
    const wchar_t* title;
};
constexpr ModuleSpec kSpecs[] = {
    {L"stage", L"\uE7C4", L"Stage Manager"},
    {L"energysaver", L"\uEC0A", L"Energy saver"},
    {L"darkmode", L"\uE708", L"Dark mode"},
    {L"hotspot", L"\uE88A", L"Mobile hotspot"},
};

const ModuleSpec* Spec(const std::wstring& id)
{
    const auto it = std::ranges::find_if(kSpecs, [&](const ModuleSpec& s) { return id == s.id; });
    return it == std::end(kSpecs) ? nullptr : &*it;
}

D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { return D2D1::ColorF(c.r, c.g, c.b, c.a * a); }

// The system's switches; may block (worker thread).
std::optional<bool> ReadSwitch(const std::wstring& id)
{
    if (id == L"energysaver")
        return ReadEnergySaver();
    if (id == L"darkmode")
        return ReadDarkMode();
    if (id == L"hotspot")
        return ReadMobileHotspot();
    return std::nullopt;
}

bool SetSwitch(const std::wstring& id, bool on)
{
    if (id == L"energysaver")
        return SetEnergySaver(on);
    if (id == L"darkmode")
        return SetDarkMode(on);
    return id == L"hotspot" && SetMobileHotspot(on);
}

} // namespace

void ControlCenter::SetModules(std::vector<std::wstring> ids)
{
    std::erase_if(ids, [](const std::wstring& id) { return !Spec(id); });
    if (ids == modules_)
        return;
    modules_ = std::move(ids);
    if (surface_.Visible())
        Close(); // its size changes: the next opening lays it out anew
}

float ControlCenter::ModulesHeight() const { return float((modules_.size() + 1) / 2) * (kModuleH + kGap); }

std::vector<D2D1_RECT_F> ControlCenter::ModuleRects() const
{
    const float colW = (kW - kGap) / 2;
    std::vector<D2D1_RECT_F> rects;
    for (size_t i = 0; i < modules_.size(); ++i) {
        const float top = kTopH + kGap + float(i / 2) * (kModuleH + kGap);
        const bool alone = i + 1 == modules_.size() && i % 2 == 0;
        const float left = i % 2 ? colW + kGap : 0;
        rects.push_back(D2D1::RectF(left, top, alone ? kW : left + colW, top + kModuleH));
    }
    return rects;
}

void ControlCenter::PaintModules(Canvas& c, const Theme& t)
{
    const float modR = std::min(radius_, 22.f);
    const std::vector<D2D1_RECT_F> rects = ModuleRects();
    for (size_t i = 0; i < rects.size(); ++i) {
        const std::wstring& id = modules_[i];
        const ModuleSpec& spec = *Spec(id);
        const D2D1_RECT_F r = rects[i];
        const bool stage = id == L"stage", wide = r.right - r.left > kW / 2;
        const auto read = moduleOn_.find(id);
        const bool known = stage || read != moduleOn_.end();
        const std::optional<bool> on = stage ? std::optional<bool>(stageOn_) : known ? read->second : std::nullopt;
        const bool live = stage || on.has_value(); // unknown yet or unavailable: nothing to press
        const std::wstring hit = stage ? std::wstring(L"stage") : L"mod:" + id;
        const float h = live ? Hover(c, hit, hover_ == hit) : 0;
        if (h > 0.001f || pressed_ == hit)
            c.Fill(r, modR, pressed_ == hit ? t.press : Alpha(t.hover, h));
        const float cx = r.left + 32, cy = (r.top + r.bottom) / 2;
        const bool lit = on.value_or(false);
        c.Circle(cx, cy, 18, lit ? t.accent : Alpha(t.text, 0.16f + 0.08f * h));
        const bool halo = c.halo;
        c.halo = false;
        c.Glyph(spec.glyph, D2D1::RectF(cx - 14, cy - 14, cx + 14, cy + 14), 14, lit ? D2D1::ColorF(1, 1, 1, 0.98f) : known && !on ? t.faint : t.text);
        c.halo = halo;
        const std::wstring state = stage && wide ? (stageOn_ ? (stageRight_ ? Tr(L"On · recent apps on the right") : Tr(L"On · recent apps on the left")) : Tr(L"Off")) :
                                   !known ? std::wstring(L"…") :
                                   !on    ? Tr(L"Unavailable") :
                                   *on    ? Tr(L"On") :
                                            Tr(L"Off");
        c.Text(Tr(spec.title), D2D1::RectF(cx + 28, cy - 18, r.right - kPad, cy), 13.5f, t.text, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        c.Text(state, D2D1::RectF(cx + 28, cy, r.right - kPad, cy + 18), 11.5f, t.subtext);
        if (live)
            hits_.push_back({r, hit});
    }
}

void ControlCenter::ReadModules()
{
    std::vector<std::wstring> ids = modules_;
    std::erase(ids, L"stage");
    if (ids.empty())
        return;
    slowWorker_.Post(L"modules", [this, ids = std::move(ids)] {
        std::map<std::wstring, std::optional<bool>> read;
        for (const std::wstring& id : ids)
            read[id] = ReadSwitch(id);
        PostResult(WM_CC_MODULES, [&] { moduleSlot_ = std::move(read); });
    });
}

void ControlCenter::FlipModule(const std::wstring& id)
{
    const auto it = moduleOn_.find(id);
    if (it == moduleOn_.end() || !it->second || moduleBusy_.contains(id))
        return;
    const bool want = !*it->second;
    it->second = want; // shown flipped at once
    moduleBusy_.insert(id);
    surface_.Invalidate();
    slowWorker_.Post({}, [this, id, want] {
        if (!SetSwitch(id, want))
            Log(L"control: %s could not be switched %s", id.c_str(), want ? L"on" : L"off");
        std::optional<bool> now = ReadSwitch(id);
        // Plugged in, Windows keeps energy saver to its own switch (the threshold only counts on battery):
        // the threshold goes back and its settings page opens instead.
        if (id == L"energysaver" && want && now == false) {
            SetEnergySaver(false);
            ShellExecuteW(nullptr, L"open", L"ms-settings:batterysaver", nullptr, nullptr, SW_SHOWNORMAL);
        }
        PostResult(WM_CC_MODULES, [&] {
            if (!moduleSlot_)
                moduleSlot_.emplace();
            (*moduleSlot_)[id] = now;
        });
    });
}

void ControlCenter::TakeModules()
{
    auto read = TakeResult(moduleSlot_);
    if (!read)
        return;
    for (auto& [id, on] : *read) {
        moduleOn_[id] = on;
        moduleBusy_.erase(id);
    }
    if (surface_.Visible())
        surface_.Invalidate();
}

} // namespace atrium
