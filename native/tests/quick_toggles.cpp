#include "shell/quick_toggles.h"

#include <iostream>
#include <vector>

using namespace atrium;

namespace {

bool Check(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool CheckNightLightEncoding()
{
    // Captured from this PC (Windows 11 build 26300) in OFF and ON states.
    const std::vector<uint8_t> capturedOff = {
        0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0x9E, 0xD3, 0xE3, 0xD4,
        0x06, 0x2A, 0x2B, 0x0E, 0x05, 0x43, 0x42, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const std::vector<uint8_t> capturedOn = {
        0x43, 0x42, 0x01, 0x00, 0x0A, 0x02, 0x01, 0x00, 0x2A, 0x06, 0xC3, 0xFE, 0x8E, 0xD8, 0x06,
        0x2A, 0x2B, 0x0E, 0x07, 0x43, 0x42, 0x01, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    bool ok = true;
    ok &= Check(IsNightLightOn(capturedOff) == false, "captured OFF record parses as off");
    ok &= Check(IsNightLightOn(capturedOn) == true, "captured ON record parses as on");
    ok &= Check(!IsNightLightOn({}).has_value(), "empty record returns nullopt");

    const auto toOn = ToggleNightLightData(capturedOff, true, 1795407683ULL);
    ok &= Check(toOn == capturedOn, "OFF -> ON produces exact captured ON record");

    const auto toOff = ToggleNightLightData(capturedOn, false, 1795407685ULL);
    ok &= Check(IsNightLightOn(toOff) == false, "ON -> OFF parses as off");
    ok &= Check(toOff.size() == capturedOff.size(), "ON -> OFF restores 27-byte length");
    ok &= Check(toOff[18] == 0x05, "ON -> OFF decrements length byte back to 0x05");

    const auto bumped = ToggleNightLightData(capturedOff, true, 1);
    ok &= Check(IsNightLightOn(bumped) == true, "stale timestamp toggle still turns on");
    ok &= Check(bumped[10] != capturedOff[10], "stale timestamp is bumped");
    return ok;
}

} // namespace

// Only the record's encoding: what this PC's switches are set to is not the tests' business.
int main()
{
    const bool ok = CheckNightLightEncoding();
    if (!ok)
        return 1;
    std::cout << "quick_toggles tests passed\n";
    return 0;
}
