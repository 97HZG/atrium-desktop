#include "shell/messengers.h"
#include "shell/wechat_source.h"

#include <iostream>
#include <stdexcept>

using namespace atrium;

namespace {

void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void CheckWeChatTime()
{
    Require(IsWeChatTime(L"17:06"), "24h clock time");
    Require(IsWeChatTime(L"10/27"), "month/day date");
    Require(IsWeChatTime(L"2025/10/27"), "slash date");
    Require(IsWeChatTime(L"2025-10-27"), "dash date");
    Require(IsWeChatTime(L"刚刚"), "just now");
    Require(IsWeChatTime(L"昨天"), "yesterday in Chinese");
    Require(IsWeChatTime(L"前天"), "day before yesterday in Chinese");
    Require(IsWeChatTime(L"星期三"), "weekday in Chinese");
    Require(IsWeChatTime(L"Yesterday"), "yesterday in English");
    Require(IsWeChatTime(L"3:45 PM"), "12h PM time");
    Require(IsWeChatTime(L"11:05 am"), "12h am time");
    Require(IsWeChatTime(L"5分钟前"), "minutes ago");
    Require(IsWeChatTime(L"2小时前"), "hours ago");
    Require(IsWeChatTime(L"2025年10月27日"), "CJK full date");
    Require(!IsWeChatTime(L""), "empty string is not a time");
    Require(!IsWeChatTime(L"你好"), "greeting text is not a time");
    Require(!IsWeChatTime(L"[图片]"), "media tag is not a time");
    Require(!IsWeChatTime(L"---"), "punctuation alone is not a time");
    Require(!IsWeChatTime(L"1") && !IsWeChatTime(L"666") && !IsWeChatTime(L"123456"), "pure numbers are not times");
}

void CheckWeChatRows()
{
    Require(IsCollapsedWeChatRow(L"折叠的群聊"), "collapsed group chats");
    Require(IsCollapsedWeChatRow(L"折叠置顶聊天"), "collapsed pinned chats");
    Require(IsCollapsedWeChatRow(L"折叠的聊天"), "collapsed chats");
    Require(IsCollapsedWeChatRow(L"Collapsed Chats"), "English collapsed chats");
    Require(IsCollapsedWeChatRow(L"Folded Group Chats"), "English folded group chats");
    Require(!IsCollapsedWeChatRow(L"产品讨论群"), "normal group is not collapsed");
    Require(IsOfficialWeChatRow(L"公众号") && IsOfficialWeChatRow(L" 服务号 ") && IsOfficialWeChatRow(L"Official Accounts"), "official accounts' rows");
    Require(!IsOfficialWeChatRow(L"公众号运营群") && !IsOfficialWeChatRow(L"Alice"), "a chat is not the official accounts' row");
    Require(ParseWeChatRow(L"订阅号消息\n[3条] 某号: 今日推送\n17:06").official, "an official accounts' row parses as such");

    const WeChatRow plain = ParseWeChatRow(L"Alice\n你好吗\n17:06");
    Require(plain.chat == L"Alice" && plain.summary == L"你好吗" && plain.unread == 0 && !plain.muted && !plain.collapsed, "plain text row");

    const WeChatRow sticker = ParseWeChatRow(L"Alice\n[1条] [动画表情]\n17:06");
    Require(sticker.unread == 1 && sticker.summary == L"[动画表情]" && !sticker.muted, "inline sticker after unread tag");

    const WeChatRow splitLink = ParseWeChatRow(L"Bob\n[2条]\n[链接] 标题\n3:45 PM");
    Require(splitLink.unread == 2 && splitLink.summary == L"[链接] 标题", "split unread line and link summary");

    const WeChatRow mention = ParseWeChatRow(L"工作群\n[有人@我]\n[1条] 请看下文档\n5分钟前");
    Require(mention.unread == 1 && mention.summary == L"请看下文档", "mention badge before unread tag");

    const WeChatRow pinnedFile = ParseWeChatRow(L"工作群\n已置顶\n[2条] [文件] report.pdf\n昨天");
    Require(pinnedFile.unread == 2 && pinnedFile.summary == L"[文件] report.pdf", "pinned badge before unread file tag");

    const WeChatRow enUnread = ParseWeChatRow(L"Team\n[3 unread] Standup at 4\nYesterday");
    Require(enUnread.unread == 3 && enUnread.summary == L"Standup at 4", "English unread tag");

    const WeChatRow enMessages = ParseWeChatRow(L"Team\n[2 messages]\n[图片]\n10:20 AM");
    Require(enMessages.unread == 2 && enMessages.summary == L"[图片]", "English messages tag with image");

    const WeChatRow saturated = ParseWeChatRow(L"大群\n[99+条] 收到\n刚刚");
    Require(saturated.unread == 99 && saturated.summary == L"收到", "99+ saturated unread count");

    const WeChatRow muted = ParseWeChatRow(L"通知群\n[5条] 周报提醒\n消息免打扰\n10:00");
    Require(muted.unread == 5 && muted.muted && !muted.collapsed && muted.summary == L"周报提醒", "muted row");

    const WeChatRow collapsed = ParseWeChatRow(L"折叠的群聊\n[4条] 群A: 收到\n2小时前");
    Require(collapsed.collapsed && collapsed.muted && collapsed.unread == 4 && collapsed.summary == L"群A: 收到", "collapsed row is muted");

    const WeChatRow collapsedPinned = ParseWeChatRow(L"折叠置顶聊天\n[1条] 群B: 测试\n2025年10月27日");
    Require(collapsedPinned.collapsed && collapsedPinned.muted && collapsedPinned.unread == 1, "collapsed pinned row is muted");

    const WeChatRow fallbackUsed = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"[语音]");
    Require(fallbackUsed.unread == 1 && fallbackUsed.summary == L"[语音]", "fallback text fills missing summary");

    const WeChatRow fallbackIgnored = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"Alice");
    Require(fallbackIgnored.unread == 1 && fallbackIgnored.summary.empty(), "fallback matching chat name is ignored");

    const WeChatRow fallbackTimeIgnored = ParseWeChatRow(L"Alice\n[1条]\n10:00", L"10:00");
    Require(fallbackTimeIgnored.unread == 1 && fallbackTimeIgnored.summary.empty(), "fallback matching time is ignored");

    const WeChatRow numericSummary = ParseWeChatRow(L"Alice\n[1条] 666\n17:06");
    Require(numericSummary.unread == 1 && numericSummary.summary == L"666", "pure numeric summary is not dropped as time");
}

void CheckWeChatDiff()
{
    std::map<std::wstring, WeChatChatState> s{{L"off", {2, L"old"}}, {L"big", {99, L"m1"}}};
    Require(DiffWeChatRows({{L"a", {L"A", L"hi", 0}}}, false, false, s).empty() && s[L"off"].unread == 2, "keep");
    Require(DiffWeChatRows({{L"off", {L"Off", L"old", 2}}}, true, false, s).empty(), "no re-fire");
    auto c = DiffWeChatRows({{L"off", {L"Off", L"old", 0}}}, false, false, s);
    Require(c.size() == 1 && !c[0].unread && s[L"off"].unread == 0, "unread -> 0 clears");
    Require(DiffWeChatRows({{L"big", {L"Big", L"m1", 99}}}, true, false, s).empty(), "99+ same");
    auto sat = DiffWeChatRows({{L"big", {L"Big", L"m2", 99}}}, true, false, s);
    Require(sat.size() == 1 && sat[0].unread && sat[0].body == L"m2", "99+ new summary");
}

} // namespace

int main()
{
    try {
        auto require = [](bool condition, const char* message) {
            if (!condition)
                throw std::runtime_error(message);
        };
        require(MessengerOfProgram(L"D:\\Download App\\Tencent\\Weixin\\Weixin.exe") == Messenger::WeChat, "WeChat 4 by its program");
        require(MessengerOfProgram(L"c:\\program files\\wxwork\\WXWork.exe") == Messenger::WeCom, "WeCom by its program");
        require(MessengerOfProgram(L"D:\\Tencent\\WXWorkLocalPro\\WXWorkLocalPro.exe") == Messenger::WeCom, "WeCom private edition");
        require(MessengerOfProgram(L"DingTalk.exe") == Messenger::DingTalk, "DingTalk by its file name");
        require(MessengerOfProgram(L"C:\\Windows\\explorer.exe") == Messenger::None, "other programs are none of them");
        require(MessengerOfNotice(L"windows", L"QQ", L"QQ") == Messenger::QQ, "QQ's Windows notification (AUMID QQ)");
        require(MessengerOfNotice(L"windows", L"\u9489\u9489", L"") == Messenger::DingTalk, "a notification by its shown name");
        require(MessengerOfNotice(L"wechat", L"WeChat", L"") == Messenger::WeChat, "the WeChat reader's card");
        require(MessengerOfNotice(L"windows", L"Mail", L"microsoft.windowscommunicationsapps_8wekyb3d8bbwe!microsoft.windowslive.mail") == Messenger::None,
                "other apps' notifications follow notifyListener");
        require(MessengerOfNotice(L"windows", L"QQ Music", L"QQMusic") == Messenger::None, "a name only containing QQ is not QQ");

        MessengerAlerts alerts;
        require(alerts.Flash(Messenger::WeCom, 1000), "a first flash begins a burst");
        require(!alerts.Flash(Messenger::WeCom, 1500), "flashes within a burst add nothing");
        require(alerts.Due(3000).empty(), "nothing is due before the wait");
        auto due = alerts.Due(4000);
        require(due.size() == 1 && due[0] == Messenger::WeCom, "a burst with no card gets a plain one");
        require(alerts.Due(9000).empty(), "once");
        require(!alerts.Flash(Messenger::WeCom, 9000), "a flash soon after the last is the same burst");
        require(alerts.Flash(Messenger::WeCom, 30000), "after a quiet spell a new burst");
        alerts.Noticed(Messenger::WeCom, 31000);
        require(alerts.Due(34000).empty(), "a card with the text follows: no plain card");
        alerts.Noticed(Messenger::QQ, 50000);
        require(alerts.Flash(Messenger::QQ, 52000), "a QQ burst");
        require(alerts.Due(56000).empty(), "a card just before the flash counts too");
        bool waiting = false;
        require(alerts.Flash(Messenger::WeChat, 60000) && alerts.Due(61000, &waiting).empty() && waiting, "still waiting");
        alerts.Cancel(Messenger::WeChat);
        require(alerts.Due(65000, &waiting).empty() && !waiting, "cancelled burst produces no plain card");
        require(alerts.Flash(Messenger::WeChat, 66000), "flash after cancel begins a new burst");

        CheckWeChatTime();
        CheckWeChatRows();
        CheckWeChatDiff();
        std::cout << "messengers: all checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "messengers: " << e.what() << "\n";
        return 1;
    }
}
