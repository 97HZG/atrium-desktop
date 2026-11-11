#include "shell/tray_click.h"
#include <iostream>
#include <stdexcept>
using namespace atrium::tray;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main()
{
    try {
        auto fail = [](Route) { return int32_t(-1); };
        auto unseen = [](bool) { return false; };
        auto r = Dispatch(Action::Left, true, fail, unseen);
        Check(r.attempts.size() == 3 && r.attempts[1].route == Route::Toolbar && r.attempts[2].route == Route::LegacyDefault, "classic left fallback order");
        r = Dispatch(Action::Right, true, fail, unseen);
        Check(r.attempts.size() == 2 && r.attempts.back().route == Route::Toolbar, "classic right messages after failed context menu");
        r = Dispatch(Action::Left, false, [](Route) { return int32_t(0); }, unseen);
        Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1, "S_OK without evidence is not success or safe to repeat");
        r = Dispatch(Action::Left, true, [](Route) { return providerTimeout; }, unseen);
        Check(r.outcome == Outcome::AcceptedUnconfirmed && r.attempts.size() == 1, "timeout does not prove the asynchronous action was cancelled");
        r = Dispatch(Action::Right, false, fail, [](bool) { return true; });
        Check(r.outcome == Outcome::Observed && r.attempts.size() == 1, "observed response wins even with provider error");
        r = Dispatch(Action::Double, false, fail, unseen);
        Check(r.attempts.empty() && r.outcome == Outcome::Unavailable, "XAML double click cannot be emulated by Invoke twice");
        r = Dispatch(Action::Double, true, [](Route route) { Check(route == Route::Toolbar, "classic double route"); return int32_t(0); }, [](bool) { return true; });
        Check(r.outcome == Outcome::Observed, "classic double response");
        std::cout << "tray dispatch: all scenarios passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
