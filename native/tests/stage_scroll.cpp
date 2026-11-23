#include "ui/stage_merge.h"
#include "ui/stage_scroll.h"
#include "ui/stage_open.h"

#include <iostream>
#include <stdexcept>

using namespace atrium;

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckMerge()
{
    Require(StageEntryAt({}, 80, 100) == -1, "empty strip hits nothing");
    const std::vector<D2D1_RECT_F> left = {StageBoxRect(kStageStripW, 40, false), StageBoxRect(kStageStripW, 40 + kStageBoxH + kStageSlotGap, false)};
    Require(StageEntryAt(left, 100, 90) == 0 && StageEntryAt(left, 100, 240) == 1, "centres hit their strip cards");
    Require(StageEntryAt(left, left[0].left - 14, left[0].bottom + 23.5f) == 0, "icon overhang padding still hits its card");
    Require(StageEntryAt(left, 1, 90) == -1 && StageEntryAt(left, 100, left[0].bottom + 25) == -1, "snap edge and slot gap hit nothing");
    const std::vector<D2D1_RECT_F> right = {StageBoxRect(kStageStripW, 40, true)};
    Require(StageEntryAt(right, right[0].left + 20, 90) == 0 && StageEntryAt(right, kStageStripW - 1, 90) == -1, "right strip hits card, not snap edge");
    RECT frame{10, 100, 710, 600};
    Require(StageBesideRect(frame, RECT{0, 50, 238, 900}, RECT{0, 0, 1920, 1040}, false) && frame.left == 238 && frame.right == 938,
            "window dropped over left strip shifts beside it");

    using Set = std::vector<std::wstring>;
    Set stage = {L"notepad.exe|1"};
    std::vector<Set> recent = {{L"calc.exe|2"}, {L"msedge.exe|3", L"code.exe|4"}};
    Require(StageMergeGroups(stage, recent, DefaultStageExcluded(), L"notepad.exe|1", {L"code.exe|4", L"msedge.exe|3"}) &&
                stage == Set{L"msedge.exe|3", L"code.exe|4", L"notepad.exe|1"} && recent == std::vector<Set>{{L"calc.exe|2"}},
            "lone stage window joins target group and emptied source disappears");

    stage = {L"a.exe|1", L"b.exe|2"};
    recent = {{L"c.exe|3"}, {L"d.exe|4"}};
    Require(StageMergeGroups(stage, recent, {}, L"a.exe|1", {L"d.exe|4"}) && stage == Set{L"d.exe|4", L"a.exe|1"} &&
                recent == std::vector<Set>{{L"b.exe|2"}, {L"c.exe|3"}},
            "multi-window stage parks remaining window at front of strip");

    stage = {L"front.exe|9"};
    recent = {{L"lone.exe|5"}, {L"pair.exe|6", L"pair.exe|7"}, {L"dest.exe|8"}};
    Require(StageMergeGroups(stage, recent, {}, L"lone.exe|5", {L"dest.exe|8"}) && stage == Set{L"dest.exe|8", L"lone.exe|5"} &&
                recent == std::vector<Set>{{L"front.exe|9"}, {L"pair.exe|6", L"pair.exe|7"}},
            "prior active group parks at front and emptied strip group goes away");

    const Set keepStage = stage;
    const std::vector<Set> keepRecent = recent;
    Require(!StageMergeGroups(stage, recent, {L"wechat.exe"}, L"d:\\apps\\wechat\\wechat.exe|10", {L"front.exe|9"}), "stageExcluded window never merges");
    Require(!StageMergeGroups(stage, recent, DefaultStageExcluded(), L"c:\\windows\\system32\\snippingtool.exe|11", {L"front.exe|9"}),
            "default excluded screenshot window never merges");
    Require(!StageMergeGroups(stage, recent, {}, L"other.exe|12", {L"missing.exe|99"}), "unknown target changes nothing");
    Require(!StageMergeGroups(stage, recent, {}, L"front.exe|9", {L"front.exe|9"}), "self-only group cannot merge into itself");
    Require(stage == keepStage && recent == keepRecent, "rejected merges leave stage and strip groups untouched");
}

} // namespace

int main()
{
    try {
        auto require = Require;
        StageOpenIntent intent;
        intent.Arm(L"editor", 100);
        require(!intent.Matches(L"browser", 101), "a launch intent never targets another app");
        require(intent.Matches(L"editor", 10099), "the requested app joins when its window arrives");
        require(!intent.Matches(L"editor", 10100), "a stale launch intent expires");
        intent.Arm(L"editor", 20000);
        intent.Cancel();
        require(!intent.Matches(L"editor", 20001), "disable or stage switching cancels a launch intent");
        StageScroll scroll;
        require(!scroll.Wheel(-120, false, 0) && scroll.Offset() == 0, "empty strip does not scroll");
        require(!scroll.Wheel(-120, false, 1), "one group does not scroll");
        require(scroll.Wheel(-120, false, 10) && scroll.Offset() == 1, "wheel down advances beyond the visible page");
        require(scroll.Index(5, 10) == 6, "viewport indexes the entire list, not just six cards");
        require(scroll.Wheel(240, false, 10) && scroll.Offset() == 9, "wheel up wraps past the start");
        require(scroll.Index(1, 10) == 0, "viewport wraps at the end");
        scroll.Reset();
        for (int i = 0; i < 3; ++i)
            require(!scroll.Wheel(-30, false, 8), "partial notches must accumulate");
        require(scroll.Wheel(-30, false, 8) && scroll.Offset() == 1, "four precision deltas equal one notch");
        require(!scroll.Wheel(30, false, 8), "opposite partial delta waits");
        require(!scroll.Wheel(-30, false, 8), "reversing partial input cancels its remainder");
        require(scroll.Wheel(120, true, 8) && scroll.Offset() == 2, "horizontal right advances");
        require(scroll.Wheel(-360, true, 8) && scroll.Offset() == 7, "horizontal left wraps");
        scroll.Fit(3);
        require(scroll.Offset() == 1, "shrinking list normalizes offset");
        scroll.Anchor(2, 3);
        require(scroll.Index(0, 3) == 2, "rebuild preserves the first visible identity");
        scroll.Reset();
        require(scroll.Offset() == 0 && !scroll.Wheel(-90, false, 3), "reset clears both offset and fractional input");
        require(scroll.Wheel(-32760, false, 8) && scroll.Offset() < 8, "large wheel input stays bounded");
        scroll.Fit(0);
        require(scroll.Offset() == 0 && scroll.Index(4, 0) == 0, "empty rebuild is safe");
        CheckMerge();
        std::cout << "PASS: cyclic Stage scrolling, strip hit-testing and group merges\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
