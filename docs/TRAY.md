# Tray：数据依据、点击与验收

本次只改托盘读取、分发及面板交互。`tray_match.h/.cpp`、`tray_click.h` 不包含 Windows 头文件；`tray_model.cpp` 是 Win32/UIA 适配层。**公开接口不能提供托盘图标的完整身份、实时位图和应用动作回执，所以这里记录证据及不确定性，不承诺任意 Windows 版本、任意程序的完美镜像。**

## 原流程与本次修正

原流程是 UIA 按钮 → 注册表记录 → 运行进程/窗口 → GetRect 存活检测 → 几何、名字、缓存顺序匹配 → 补充记录 → 生成图标 → 点击。原来的顺序动态规划可把任意名字配到记录；精确提示文字还可使用死亡记录。补充阶段按 exe、名字、矩形去重，并且发现任何一个溢出按钮就停止补充。点击先经典工具栏消息，否则 Invoke/ShowContextMenu；请求返回成功即记作成功。找不到按钮时会打开再收起溢出区，随后操作重新折叠的元素。

保留 `2d034cd` 的 sharedPlace 修复意图，并扩大到**同 exe 的不同身份**：矩形不证明身份，exe 和提示文字也不证明两个图标是同一个。

## 刷新各阶段的输入、输出和依据

| 阶段 | 输入 | 输出与依据 |
| --- | --- | --- |
| UIA 枚举 | Shell_TrayWnd、XAML 溢出窗口/内容桥 | `NotifyItemIcon` 的提示、矩形、分区、RuntimeId、元素；按 UIA 顺序。不存在 XAML 按钮时读取 TrayNotifyWnd/SysPager 与 NotifyIconOverflowWindow 下 ToolbarWindow32 的 Button。不按本地化名称识别控件，不打开任何窗口。 |
| 记录 | HKCU\Control Panel\NotifyIconSettings | ExecutablePath、InitialTooltip、IconSnapshot、UID/GUID、IsPromoted、UIOrderList。全部是缓存元数据，存在记录不表示图标存活。 |
| 存活 | GUID，或运行程序的顶层/消息窗口 + UID | GUID 用空 HWND 调 GetRect，只证明 GUID 存在，绝不证明窗口/PID 归属。UID 只有在实际窗口通过 GetRect 后才生成 `uid:PID:HWND:uID`。不以文件名相同替代完整进程路径验证。 |
| 规范化 | 活记录、真实身份 | 同一个真实身份的旧/新记录合并，优先 UIOrderList 中更前的记录，再按记录 key 稳定排序；不同身份永不按 exe/提示/位置合并。缺少身份或检测失败的记录不能补充或提供记录图片。 |
| 几何匹配 | 按钮矩形、活记录矩形 | 记录中心落在按钮内，且在两个方向均唯一，才能匹配。多个不同身份共享的矩形在消费任何候选之前标记 sharedPlace；永不参与几何匹配，也不成为“占用位置”。 |
| 名字匹配 | 当前提示第一行、缓存提示、文件描述/产品名/文件名、固定应用名、窗口标题 | 唯一的双向最佳匹配，精确优先，再使用程序名前缀（如 Steam - 正在同步）。窗口标题只允许精确匹配，防止浏览器页面冒领程序。ASCII 忽略大小写，其他字符保留。平分不选第一个。 |
| 顺序匹配 | 各分区的按钮顺序、规范化活记录的 UIOrderList | 仅在完整分区数量相等、每个记录均有唯一顺序值、所有已确定的几何/名字锚点一致时使用。支持空提示、完全变化的状态提示，以及记录和当前提示语言不同。顺序是启发式依据，会明确写进日志；不把部分按钮列表与更长的记录列表直接对齐。 |
| 运行程序匹配 | 未匹配按钮、运行程序元数据 | Windows 10 等无记录场景使用唯一最佳名字提供 exe 图片。进程存在本身不生成图标；没有按钮也没有存活身份就不显示。只有需要此阶段时才读取额外程序版本信息。 |
| 补充 | 尚未匹配的规范化活身份 | 按 UIOrderList 补充，包括完全未创建和部分创建的溢出树。补充项没有 UIA 元素，也没有可点击的假矩形。未知 UIA 按钮保持未知，不用位置编号猜程序。 |
| 图标快照 | 配对结果、PNG 缓存/程序文件图片 | 生成 TrayIcon，保存来源、匹配方式、真实身份和 RuntimeId。图片按记录 key/PNG 内容缓存；不宣称缓存图片是当前动画/状态角标的实时截图。 |

顺序/名称无法解决的歧义仍是边界：例如部分 UIA 树内的无名按钮和若干同名活记录，同时缺少可靠几何或顺序。此时保留未知按钮和活记录可能多显示占位项；擅自合并会造成另一种漏项或错配。当前实现没有用隐藏的跨进程内存读取来绕过此信息缺失。这个场景**尚不满足绝对“不多、不漏、不配错”**，需要真实样本继续改进。

记录路径本身也是 Explorer 缓存的关联，不是 GUID 的所有权证明。对于无法查询完整路径的受保护 UID 程序、把图标注册到未枚举子窗口的程序，可能无法补齐尚未物化的图标。Windows 10 的未知/空提示也无法普遍推断程序。对这些情况不使用旧记录“复活”。

## 点击分发

1. 工作线程重新读取并匹配。已物化按钮优先核对原 RuntimeId，已有的真实身份和 exe 必须同时一致；不会因记录被重新配到另一个按钮而跳转。未物化记录使用唯一的存活身份；显式程序唤起请求仅允许唯一 exe 候选。PID、进程创建时间、任务栏 HWND 共同限定 shell 实例，跨实例点击拒绝自动分发；跨实例刷新丢弃重读。纯函数 `tray_target.h` 同时供点击和双击手势校验使用。
2. 左键尝试 UIA Invoke；右键尝试 ShowContextMenu。明确失败后，经典路径发送工具栏消息；左键最后可尝试 MSAA LegacyIAccessible 的 DoDefaultAction。
3. 经典工具栏重新核对类名、shell PID、根窗口、当前按钮矩形及转换后的客户区坐标。顺序为 MOVE、DOWN、UP；双击为 MOVE、LDOWN、LUP、LDBLCLK、LUP。只传整数坐标，无远程结构体或指针。PostMessage 只证明排队成功，不证明程序已响应。
4. 每次动作前后，按完整 exe 路径核对程序的可见窗口、前台窗口及菜单状态。新窗口、新菜单，或左键引起前台窗口变化记作 `observed`。这是有限时间窗内的响应证据，不是应用协议回执；自绘无新 HWND 的菜单、不同 exe 的辅助进程、后台状态变化、较慢响应可能无法确认。
5. `S_OK` 或 UIA 超时但没有证据记为 `accepted-unconfirmed`，不通过下一种 API 重复点击，避免切换类动作反转。其他错误若已经观察到响应，也停止重试。只有明确的 ELEMENTNOTAVAILABLE 且此前确实存在元素时，重新解析一次；不会把所有 E_FAIL 都认定为元素过期。
6. 左键/双击失败时只允许恢复该 exe **已经存在且可见或最小化的普通窗口**，并检查是否成为前台。不显示隐藏主窗口，不 ShellExecute，不启动 exe。右键不以打开主窗口代替程序菜单。
7. 没有可靠响应时打开系统溢出区，并提示用户直接操作。Win11 使用非本地化结构签名（已知通知图标之前唯一的 NormalButton/SystemTrayIcon，后面仍存在普通系统按钮），签名缺失/不唯一则拒绝猜测；Win10 使用 TrayNotifyWnd 下唯一 Button 的 BM_CLICK。只有溢出按钮确实非离屏、有矩形、WindowFromPoint 命中溢出窗口才报告回退打开成功。没有可靠入口时显示提示，请用户暂时关闭隐藏任务栏后操作。没有 Win+B、鼠标 SendInput、移动光标、短暂显示任务栏或打开再收起的后台准备。

Win11 的结构签名不是微软公开的稳定接口；没有直接显示的图标或布局不同的版本可能不能自动打开。这时给出明确提示，不点击输入法等其他 SystemTrayIcon。BM_CLICK 是公开按钮消息，但经典系统托盘结构同样需要 Win10 实机确认。

双击不是 Invoke 两次。面板用系统 GetDoubleClickTime/空间阈值识别完整手势，左键因此等待系统双击时间（通常 500 ms），按下反馈仍立即显示。经典路径发送真正的双击消息；XAML 无公开双击 Pattern，转已有窗口/系统托盘。关闭面板取消待发送手势；按下时保存图标身份，刷新重新排列不会改变目标。

## E_FAIL 与 S_OK 的诊断

微软说明 Invoke 的行为取决于 provider；隐藏/阻挡或不支持操作可能失败，但没有公开规定“托盘 E_FAIL 必然由 SW_HIDE 导致”。ShowContextMenu 可以调用元素的**父元素菜单**，因此它的 S_OK 不等于程序菜单已出现。[Invoke 文档](https://learn.microsoft.com/en-us/dotnet/api/system.windows.automation.invokepattern.invoke)、[ShowContextMenu 文档](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationelement3-showcontextmenu)。

每次点击记录 taskbarVisible、autoHide、overflowExists、overflowVisible、IsOffscreen、IsEnabled、属性 HRESULT、来源/匹配、身份、每次尝试 HRESULT、观察结果和最终结果。它们分别区分：任务栏 SW_HIDE、自动隐藏、溢出树未创建、内容折叠、元素失效、请求被接受但无法确认。IsOffscreen 不能代替宿主可见性：本机隐藏任务栏上的 OneDrive 按钮仍报告 offscreen=0。

2026-11-11 本机没有复现 E_FAIL，因此没有把某个原因写成已证实的唯一根因。已复现 Steam 的 Invoke/ShowContextMenu 返回 S_OK 但没有可观察响应；显示溢出窗口 HWND 后，其按钮仍 offscreen=1、矩形全零。调用经过结构验证的 chevron 后，内容才可见并能通过落点核验。修复针对已证实的“请求接受不等于成功”和“只显示 HWND 不等于物化内容”；其他 E_FAIL 环境还需要新日志与对照实验。

参考：[GetRect](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicongetrect)、[Legacy DoDefaultAction](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationlegacyiaccessiblepattern-dodefaultaction)、[UIA 错误码](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-error-codes)、[BM_CLICK](https://learn.microsoft.com/en-us/windows/win32/controls/bm-click)。

## 开源实现对照（2026-11-12）

本次阅读固定版本的实现，按机制独立实现所需防护，没有复制第三方源码：

- [ManagedShell ExplorerTrayService](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/ExplorerTrayService.cs) 用远程工具栏缓冲区和 ReadProcessMemory 读取 HWND、UID、回调消息及版本；与 Atrium 禁止跨进程内存访问的约束冲突。
- [ManagedShell TrayService](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/TrayService.cs) 注册自己的 Shell_TrayWnd，接收 WM_COPYDATA，并调整窗口顺序、广播 TaskbarCreated。这是接管托盘宿主的架构，不能当作透明读取 Explorer 的 API 使用。
- [ManagedShell NotifyIcon](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/NotifyIcon.cs) 根据已知 CallbackMessage、UID、Version 编码鼠标及 NIN_SELECT/WM_CONTEXTMENU；[RetroBar 控件](https://github.com/dremin/RetroBar/blob/d2b0b8535a395f3f13578d2825ca2094845c1779/RetroBar/Controls/NotifyIcon.xaml.cs) 转交鼠标事件给它。可借鉴身份与事件语义，不能仅凭 exe 或 GUID 猜这些协议字段。
- [YASB trayhook](https://github.com/amnweb/yasb/blob/37928dfeacc579948c4b4bf12e169bcbf6006ff2/src/core/widgets/services/systray/hook/trayhook.cpp) 明确通过 SetWindowsHookEx 把 DLL 加载进 Explorer，再替换窗口过程捕获注册数据及图片；不适用于 Atrium 的无注入约束。

微软的 [NOTIFYICONDATA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw) 定义了不同版本的回调打包规则。当前公开读取途径缺少回调消息、版本和完整实时位图，因此不能把“发一个 WM_RBUTTONUP 给推测窗口”当作原生等价方案。也不能承诺无条件完美替代。

[GetRuntimeId 文档](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationelement-getruntimeid) 允许 ID 随时间复用，所以没有新增把 RuntimeId 永久缓存到 exe 的映射。已知记录要求身份交叉校验；未知按钮要求 exe/提示一致。未知按钮在同一 shell 内被同名新元素复用 ID 的情形仍没有完整身份保证。

这一轮还收紧了名字前缀：`Steam - 正在同步`、`STEAM：正在同步` 可作为状态提示，`SteamVR`、`SteamHelper`、`QQMusic` 不会仅因前缀被配给 Steam/QQ。完整分区的顺序匹配仍是单独的启发式，不因名字规则收紧就变成所有权证明。

## 性能与日志

- 所有 UIA、注册表、GetRect、进程查询、图片读取及点击确认都在 MTA worker。main 的 OpenTray 只排队，原来的 OpenTrayStep2 UI 线程跨进程调用已删除。
- 刷新请求合并；动作保持 FIFO 并排在待处理刷新之前。面板打开期间每 1.5 秒请求刷新，关闭时停止；Explorer 重启和首次启动沿用原有刷新通知。
- 优先 CUIAutomation8，连接/事务超时设为 250 ms；降级旧 UIA。它限制单次 provider 等待，不是整个刷新或所有第三方调用的绝对时间保证。动作后的观察窗为接受后 280 ms、明确错误后 40 ms；总延迟还包括读取、provider、双击等待和回退。
- 列表/来源/匹配方式有变化才打印详细清单；附记录数、活记录数、别名数、共享矩形数、未知数和 UIA 时间。未变化但超过 500 ms 也记录慢刷新。
- `observed`、`system-tray`、`needs-user` 是不同的最终结果；不能把 system-tray 统计为原生点击成功。

## 验证（2026-11-11，Windows 11 build 26300）

Release 主程序编译；纯函数测试覆盖：溢出区未打开、已打开、部分树、不同/相同 exe 共用矩形、同 exe GUID+UID、旧新记录别名、已退出残留、Win10 无记录运行程序匹配、空/变化提示、不同程序同名、窗口标题冒领、过期顺序、S_OK 未确认、超时不重复、经典分发与 XAML 双击回退。

实际重启 Explorer，保持 Atrium 隐藏任务栏且不打开溢出区：`buttons=1 reconciled=9`。随后打开溢出区：`buttons=9 reconciled=9`。没有把未重新注册的 NVIDIA/QQ 记录补回来。独立 fixture 用同一 exe 注册 GUID 和 UID 图标，实际列表随删除为 `2 → 1 → 0`；测试后清理自己注册的两个图标。

5 个实际程序的集成探针结果（调用生产 TrayModel worker，非截图人工验收；全部 taskbarVisible=0 / autoHide=1）：

| 程序 | 左键 | 右键 |
| --- | --- | --- |
| Steam | 已有窗口恢复到前台；UIA 本身未确认 | 系统托盘回退已验证 |
| 微信 | 观察到应用响应 | 观察到应用响应 |
| Tailscale | 观察到应用响应 | 观察到应用响应 |
| v2rayN | 系统托盘回退已验证 | 观察到应用响应 |
| 网易云音乐 | 观察到应用响应 | 观察到应用响应 |

常态读取/匹配约 31–187 ms；首次开发构建刷新 391 ms；Explorer 恢复过程旧 UIA 实例有一次 828 ms。上述点击常态 93–531 ms，不含面板双击等待。测试中网易云 Invoke 有一次阻塞约 2 s；因此加入 UIA2 超时，不能凭配置就宣称全场景性能已验收。测试工具未暴露 Atrium 的无边框窗口，未完成面板截图、完整鼠标手势或“从系统托盘人工点五个菜单”的验收。**Win10 实机、E_FAIL 条件对照、五个程序所有动作均原生成功的目标仍未全部验证。**

## 重现命令

```powershell
cmake -S native -B native/build/tests -A x64 -DATRIUM_BUILD_TESTS=ON
cmake --build native/build/tests --config Release --target atrium_core tray_match_tests tray_click_tests tray_live_probe
ctest --test-dir native/build/tests -C Release -R tray_ --output-on-failure
native/build/tests/tests/Release/tray_live_probe.exe
native/build/tests/tests/Release/tray_live_probe.exe --tree
native/build/tests/tests/Release/tray_live_probe.exe --fixture
native/build/tests/tests/Release/tray_live_probe.exe --click 'C:\full\path\app.exe' right
native/build/tests/tests/Release/tray_live_probe.exe --visible-click 'C:\full\path\app.exe' left
```

探针默认只读；`--fixture` 只注册/删除本探针两个图标；`--click` 明确操作已有图标，exe 候选不唯一或不存在则拒绝；`--visible-click` 先打开并核验系统溢出区用于条件对照。探针链接同一生产适配层，只有快捷方式/程序图片读取被桩替代（注册表 PNG 仍可解码）；不代表完整生产图片/UI 视觉验收。它不加入自动 CTest，以免无人值守测试改变用户前台。日志仍写入正常 core.log。
