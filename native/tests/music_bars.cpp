#include "render/music_bars.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

using namespace atrium;

namespace {

int g_failures = 0;

void Require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

// Runs `bars` for `ms` in 16 ms frames; true if the last frame still moved.
bool Run(MusicBars& bars, float peak, bool playing, float intensity, int ms)
{
    bool moving = false;
    for (int t = 0; t < ms; t += 16)
        moving = bars.Step(peak, playing, intensity, 16);
    return moving;
}

} // namespace

int main()
{
    Require(MusicBars::Loudness(0.f) == 0.f, "silence is no loudness");
    Require(MusicBars::Loudness(1.f) == 1.f, "full scale is full loudness");
    Require(MusicBars::Loudness(0.0039f) == 0.f, "48 dB down is silence");
    const float half = MusicBars::Loudness(0.0631f); // -24 dB
    Require(half > 0.49f && half < 0.51f, "loudness is linear in decibels");

    MusicBars bars;
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(bars.Height(i) == MusicBars::StillHeight(i), "a new entry shows the still waveform");
    Require(!bars.Step(0.f, false, 1.f, 16), "paused from the start: nothing moves");
    Require(MusicBars::StillHeight(2) > MusicBars::StillHeight(0) && MusicBars::StillHeight(2) > MusicBars::StillHeight(4),
            "the still waveform is tallest in the middle");

    Require(Run(bars, 0.7f, true, 1.f, 600), "playing music keeps the bars moving");
    Require(bars.Level() > 0.8f, "the level reaches a loud peak within 0.6 s");
    float low = 1.f, high = 0.f;
    for (int t = 0; t < 3000; t += 16) {
        bars.Step(0.7f, true, 1.f, 16);
        for (size_t i = 0; i < MusicBars::kCount; ++i) {
            Require(bars.Height(i) >= MusicBars::kRest - 0.001f && bars.Height(i) <= 1.f, "bars stay inside the icon");
            low = std::min(low, bars.Height(2));
            high = std::max(high, bars.Height(2));
        }
    }
    Require(high - low > 0.2f, "the middle bar swings with the music");
    bool apart = false;
    for (int t = 0; t < 1000 && !apart; t += 16) {
        bars.Step(0.7f, true, 1.f, 16);
        apart = std::abs(bars.Height(1) - bars.Height(3)) > 0.08f;
    }
    Require(apart, "bars move on their own rhythms, not in step");

    MusicBars quiet;
    Run(quiet, 0.01f, true, 1.f, 2000);
    float quietTop = 0.f;
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        quietTop = std::max(quietTop, quiet.Height(i));
    Require(quietTop < 0.6f, "quiet music moves the bars a little");

    Require(!Run(bars, 0.7f, false, 1.f, 2500), "paused: the bars settle and stop asking for frames");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::abs(bars.Height(i) - MusicBars::StillHeight(i)) < 0.005f, "paused: the bars rest in the still waveform");
    Require(!Run(bars, 0.7f, true, 0.f, 2500), "motion off: playing music leaves the bars still");

    MusicBars jumpy;
    Require(jumpy.Step(0.9f, true, 1.f, 5000), "a long frame is clamped, never a jump past the target");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(jumpy.Height(i) <= 1.f, "a long frame keeps the bars inside the icon");

    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Require(MusicBars::Loudness(qnan) == 0.f, "NaN peak produces zero loudness");
    Require(MusicBars::Loudness(inf) == 0.f, "+Inf peak produces zero loudness");
    Require(MusicBars::Loudness(-inf) == 0.f, "-Inf peak produces zero loudness");
    Require(MusicBars::Loudness(-0.5f) == 0.f, "negative peak produces zero loudness");

    MusicBars bad;
    bad.Step(qnan, true, 1.f, 16.0);
    bad.Step(inf, true, inf, inf);
    bad.Step(0.5f, true, qnan, qnan);
    Require(std::isfinite(bad.Level()), "level stays finite after NaN/Inf inputs");
    for (size_t i = 0; i < MusicBars::kCount; ++i)
        Require(std::isfinite(bad.Height(i)) && bad.Height(i) >= MusicBars::kRest && bad.Height(i) <= 1.f,
                "heights stay finite and bounded after NaN/Inf inputs");
    Require(!Run(bad, 0.f, false, 1.f, 2500), "bars settle normally after NaN/Inf inputs");

    std::cout << (g_failures ? "music_bars: FAILED" : "music_bars: ok") << '\n';
    return g_failures ? 1 : 0;
}
