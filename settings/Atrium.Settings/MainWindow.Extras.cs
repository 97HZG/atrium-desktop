using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;
using Windows.UI;

namespace Atrium.Settings;

// The pages of the bar's newer entries: the now-playing entry (page 15), the folder stack (page 16)
// and Launchpad (page 17). Each page is built here when the window first asks
// for it (ExtrasPage, ExtrasSide), its navigation item with it; its controls read the settings each
// time the page shows. The previews are module scenes: the card, and the stack as it opens.
public sealed partial class MainWindow
{
    private const int NowPlayingPage = 15, StacksPage = 16, LaunchpadPage = 17;
    private static readonly string[] MotionTags = ["off", "subtle", "normal", "vivid"];
    private static readonly string[] AppModeTags = ["all", "allow", "block"];
    private static readonly int[] StackMaxima = [6, 8, 12, 16];

    private StackPanel? _npMain, _npSide, _stMain, _stSide, _lpMain, _lpSide;
    private Border? _npStage, _stStage, _lpStage;
    private Canvas? _npCanvas, _stCanvas, _lpCanvas;
    private ModuleScene? _npScene, _stScene, _lpScene;
    private ToggleTile? _launchpadSwitch, _npSwitch, _npPausedSwitch, _npWheelSwitch, _stSwitch;
    private SegmentTrack? _npMotion, _npMode, _stLayout, _stMax;
    private TextBox? _npApps;
    private StackPanel? _stList;
    private TextBlock? _stCount;

    private StackPanel ExtrasPage(int page)
    {
        BuildExtras();
        return page switch { NowPlayingPage => _npMain!, StacksPage => _stMain!, _ => _lpMain! };
    }

    private StackPanel ExtrasSide(int page)
    {
        BuildExtras();
        return page switch { NowPlayingPage => _npSide!, StacksPage => _stSide!, _ => _lpSide! };
    }

    private ModuleScene? ExtrasScene(int page) => page switch
    {
        NowPlayingPage => _npScene ??= new NowPlayingScene(this, _npStage!, _npCanvas!),
        StacksPage => _stScene ??= new StacksScene(this, _stStage!, _stCanvas!),
        LaunchpadPage => _lpScene ??= new LaunchpadScene(this, _lpStage!, _lpCanvas!),
        _ => null,
    };

    // The bar pages' scenes and these pages' (Scenes in MainWindow.Modules.cs).
    private IEnumerable<ModuleScene> WithExtras(ModuleScene?[] scenes) => scenes.Append(_npScene).Append(_stScene).Append(_lpScene).OfType<ModuleScene>();

    // ---- Building ----

    private void BuildExtras()
    {
        if (_npMain != null) return;
        (_npMain, _npStage, _npCanvas) = StagePage();
        (_stMain, _stStage, _stCanvas) = StagePage();
        (_lpMain, _lpStage, _lpCanvas) = StagePage();
        int at = CenterStack.Children.IndexOf(MainIme) + 1;
        CenterStack.Children.Insert(at, _lpMain);
        CenterStack.Children.Insert(at, _stMain);
        CenterStack.Children.Insert(at, _npMain);
        _npSide = BuildNowPlayingSide();
        _stSide = BuildStacksSide();
        _stMain.Children.Add(BuildStacksFolders());
        _launchpadSwitch = Toggle("Show on the taskbar", "Show Launchpad on the taskbar",
            "An entry next to the start logo opens every app, page by page, in one large window of glass, as on macOS.", (s, on) => s.ShowLaunchpad = on);
        _lpSide = Side("Launchpad", _launchpadSwitch);
        InspectorStack.Children.Add(_npSide);
        InspectorStack.Children.Add(_stSide);
        InspectorStack.Children.Add(_lpSide);
        // Each page reads the settings as it shows (they may have changed elsewhere since).
        _npSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_npSide.Visibility == Visibility.Visible) LoadExtras(); });
        _stSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_stSide.Visibility == Visibility.Visible) LoadExtras(); });
        AddNavItem(NowPlayingPage, "", "Now Playing", "12");
        AddNavItem(StacksPage, "", "Folder Stacks", "10"); // the bar's order: the stack sits left of the tray
        _lpSide.RegisterPropertyChangedCallback(UIElement.VisibilityProperty, (_, _) => { if (_lpSide.Visibility == Visibility.Visible) LoadExtras(); });
        // The bar's order, left to right: the start menu, Launchpad, the stack, then the tray's side.
        AddNavItem(LaunchpadPage, "\uE71D", "Launchpad", "10");
    }

    // A page's picture: a stage the size of the other bar pages', its scene drawn on the canvas.
    private (StackPanel, Border, Canvas) StagePage()
    {
        var canvas = new Canvas { HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
        var stage = new Border
        {
            Height = 520,
            CornerRadius = new CornerRadius(28),
            Background = new SolidColorBrush(Color.FromArgb(0xFF, 0xCB, 0xD3, 0xDD)),
            Child = new Grid
            {
                Children =
                {
                    canvas,
                    new Border
                    {
                        CornerRadius = new CornerRadius(28), BorderThickness = new Thickness(1),
                        BorderBrush = new SolidColorBrush(Color.FromArgb(0x1F, 0, 0, 0)), IsHitTestVisible = false,
                    },
                },
            },
        };
        stage.SizeChanged += OnModuleStageSizeChanged;
        return (new StackPanel { Visibility = Visibility.Collapsed, Spacing = 10, Children = { stage } }, stage, canvas);
    }

    // Beside the bar's other parts in the navigation, after the item tagged `after`.
    private void AddNavItem(int page, string glyph, string text, string after)
    {
        var item = new ListViewItem
        {
            Tag = page.ToString(System.Globalization.CultureInfo.InvariantCulture),
            Content = new NavLabel { Glyph = glyph, Text = text, Sub = true },
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(item, text);
        int index = Nav.Items.IndexOf(Nav.Items.OfType<ListViewItem>().FirstOrDefault(i => i.Tag as string == after));
        Nav.Items.Insert(index < 0 ? Nav.Items.Count : index + 1, item);
    }

    private StackPanel BuildNowPlayingSide()
    {
        _npSwitch = Toggle("Show on the taskbar", "Show now playing",
            "Right of the status entry, while a music or video app is playing. Its five bars move with the sound.", (s, on) => s.ShowNowPlaying = on);
        _npPausedSwitch = Toggle("Keep while paused", "Keep now playing while paused", "", (s, on) => s.NowPlayingShowPaused = on);
        _npWheelSwitch = Toggle("Wheel changes the player's volume", "Wheel changes the player's volume",
            "The wheel over the entry turns the playing app's own volume up or down.", (s, on) => s.NowPlayingScrollVolume = on);
        _npMotion = Segments("nowplayingmotion", [("Off", "off"), ("Subtle", "subtle"), ("Standard", "normal"), ("Vivid", "vivid")],
            tag => CurrentSettings.NowPlayingMotion = tag);
        _npMode = Segments("nowplayingapps", [("All apps", "all"), ("Only these", "allow"), ("All but these", "block")], tag =>
        {
            CurrentSettings.NowPlayingAppMode = tag;
            _npApps?.IsEnabled = tag != "all";
        });
        _npApps = new TextBox
        {
            AcceptsReturn = true,
            TextWrapping = TextWrapping.NoWrap,
            Height = 104,
            PlaceholderText = Loc.T("One app a line, e.g. Spotify.exe or cloudmusic.exe"),
        };
        _npApps.LostFocus += (_, _) =>
        {
            if (_loading) return;
            CurrentSettings.NowPlayingApps = [.. _npApps.Text.Split('\r', '\n').Select(l => l.Trim()).Where(l => l.Length > 0).Take(64)];
            Changed();
        };
        return Side("Now Playing", _npSwitch, _npPausedSwitch, Row("Bars' motion", _npMotion), _npWheelSwitch, Row("Apps", _npMode), _npApps);
    }

    private StackPanel BuildStacksSide()
    {
        _stSwitch = Toggle("Show on the taskbar", "Show folder stacks",
            "Left of the tray: a click fans out the folders added here, a click on one opens it.", (s, on) => s.ShowFolderStacks = on);
        _stLayout = Segments("stacklayout", [("Fan", "fan"), ("List", "column")], tag => CurrentSettings.FolderStackLayout = tag);
        var invariant = System.Globalization.CultureInfo.InvariantCulture;
        _stMax = Segments("stackmax", [.. StackMaxima.Select(n => (n.ToString(System.Globalization.CultureInfo.CurrentCulture), n.ToString(invariant)))],
            tag => CurrentSettings.FolderStackMax = int.Parse(tag, System.Globalization.CultureInfo.InvariantCulture));
        return Side("Folder Stacks", _stSwitch, Row("Opens as", _stLayout), Row("Shows at most", _stMax));
    }

    // The folders, under the picture as the taskbar's pinned apps are under theirs: a title with the
    // count and the buttons that add, then the list in a group.
    private StackPanel BuildStacksFolders()
    {
        var res = Application.Current.Resources;
        _stList = new StackPanel { Padding = new Thickness(0, 4, 0, 4) };
        _stCount = new TextBlock { Style = (Style)res["FootnoteStyle"], VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
        var places = new MenuFlyout { Placement = Microsoft.UI.Xaml.Controls.Primitives.FlyoutPlacementMode.BottomEdgeAlignedRight };
        foreach (var (name, path) in KnownStackFolders())
        {
            var item = new MenuFlyoutItem { Text = Loc.T(name) };
            item.Click += (_, _) => AddStackFolder(path);
            places.Items.Add(item);
        }
        var common = new DropDownButton { Content = Loc.T("Common folders"), Flyout = places };
        var add = new Button { Content = Loc.T("Add folder…"), Style = (Style)res["PrimaryButtonStyle"] };
        add.Click += async (_, _) => await AddStackFolderAsync();
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right,
            Children = { _stCount, common, add },
        };
        var title = new TextBlock
        {
            Text = Loc.T("Folders"),
            Style = (Style)res["SectionStyle"],
            Margin = new Thickness(4, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var header = new Grid { Margin = new Thickness(0, 0, 0, 10), Children = { title, buttons } };
        var group = new Border { Style = (Style)res["GroupStyle"], Child = _stList };
        return new StackPanel { Margin = new Thickness(0, 16, 0, 0), Children = { header, group } };
    }

    private static (string Name, string Path)[] KnownStackFolders()
    {
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        return
        [
            ("Desktop", Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory)),
            ("Downloads", Path.Combine(home, "Downloads")),
            ("Documents", Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments)),
            ("Pictures", Environment.GetFolderPath(Environment.SpecialFolder.MyPictures)),
            ("Music", Environment.GetFolderPath(Environment.SpecialFolder.MyMusic)),
        ];
    }

    // ---- The controls ----

    private ToggleTile Toggle(string title, string name, string note, Action<BarSettings, bool> set)
    {
        var tile = new ToggleTile { Title = Loc.T(title), SwitchName = Loc.T(name), Flat = true };
        if (note.Length > 0) tile.Note = Loc.T(note);
        tile.Toggled += (_, _) =>
        {
            if (_loading) return;
            set(CurrentSettings, tile.IsOn);
            Changed();
        };
        return tile;
    }

    private SegmentTrack Segments(string group, (string Label, string Tag)[] items, Action<string> set)
    {
        var track = new SegmentTrack();
        foreach (var (label, tag) in items)
        {
            var button = new RadioButton { GroupName = group, Content = Loc.T(label), Tag = tag, Style = (Style)Application.Current.Resources["SegmentStyle"] };
            button.Checked += (_, _) =>
            {
                if (_loading) return;
                set(tag);
                Changed();
            };
            track.Children.Add(button);
        }
        return track;
    }

    private static void Check(SegmentTrack? track, string tag)
    {
        if (track == null) return;
        foreach (var button in track.Children.OfType<RadioButton>()) button.IsChecked = button.Tag as string == tag;
    }

    private static StackPanel Row(string title, FrameworkElement control) => new()
    {
        Style = (Style)Application.Current.Resources["FlatRowStyle"],
        Children = { new TextBlock { Text = Loc.T(title), Style = (Style)Application.Current.Resources["RowTitleStyle"] }, control },
    };

    private static StackPanel Side(string title, params FrameworkElement[] rows)
    {
        var inner = new StackPanel { Children = { new TextBlock { Text = Loc.T(title), Style = (Style)Application.Current.Resources["SectionStyle"] } } };
        foreach (var row in rows) inner.Children.Add(row);
        return new StackPanel { Style = (Style)Application.Current.Resources["InspectorPageStyle"], Visibility = Visibility.Collapsed, Children = { inner } };
    }

    // The controls from the settings, without counting as an edit.
    private void LoadExtras()
    {
        bool was = _loading;
        _loading = true;
        var s = CurrentSettings;
        _launchpadSwitch?.IsOn = s.ShowLaunchpad;
        _npSwitch?.IsOn = s.ShowNowPlaying;
        _npPausedSwitch?.IsOn = s.NowPlayingShowPaused;
        _npWheelSwitch?.IsOn = s.NowPlayingScrollVolume;
        Check(_npMotion, MotionTags.Contains(s.NowPlayingMotion) ? s.NowPlayingMotion : "normal");
        Check(_npMode, AppModeTags.Contains(s.NowPlayingAppMode) ? s.NowPlayingAppMode : "all");
        if (_npApps != null)
        {
            _npApps.Text = string.Join("\r", s.NowPlayingApps);
            _npApps.IsEnabled = s.NowPlayingAppMode != "all";
        }
        _stSwitch?.IsOn = s.ShowFolderStacks;
        Check(_stLayout, s.FolderStackLayout == "column" ? "column" : "fan");
        int max = StackMaxima.MinBy(n => Math.Abs(n - s.FolderStackMax));
        Check(_stMax, max.ToString(System.Globalization.CultureInfo.InvariantCulture));
        FillStackList();
        _loading = was;
    }

    // ---- The stack's folders ----

    private void FillStackList()
    {
        if (_stList == null) return;
        _stList.Children.Clear();
        var folders = CurrentSettings.FolderStacks;
        _stCount?.Text = folders.Count == 0 ? "" : Loc.F("{0} of 32", folders.Count);
        if (folders.Count == 0)
        {
            var res = Application.Current.Resources;
            _stList.Children.Add(new StackPanel
            {
                Padding = new Thickness(24, 36, 24, 36),
                Spacing = 6,
                HorizontalAlignment = HorizontalAlignment.Center,
                Children =
                {
                    new TextBlock { Text = Loc.T("No folders yet"), Style = (Style)res["TitleStyle"], HorizontalAlignment = HorizontalAlignment.Center },
                    new TextBlock
                    {
                        Text = Loc.T("Choose one with Add folder…, or pick a common folder such as Downloads."),
                        Style = (Style)res["RowNoteStyle"], TextAlignment = TextAlignment.Center, MaxWidth = 360,
                    },
                },
            });
            return;
        }
        for (int i = 0; i < folders.Count; i++) _stList.Children.Add(StackRow(i, folders[i]));
    }

    // A folder as a pinned app's row: its icon, its name over its path, and up, down and remove.
    private Grid StackRow(int index, string path)
    {
        var res = Application.Current.Resources;
        var row = new Grid { Padding = new Thickness(14, 8, 10, 8), ColumnSpacing = 14, MinHeight = 60 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var icon = new TextBlock
        {
            Text = "\U0001F4C1",
            FontFamily = new FontFamily("Segoe UI Emoji"),
            FontSize = 28,
            Width = 40,
            TextAlignment = TextAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
        };
        string name = Path.GetFileName(path.TrimEnd('\\', '/'));
        var text = new StackPanel
        {
            VerticalAlignment = VerticalAlignment.Center,
            Spacing = 1,
            Children =
            {
                new TextBlock
                {
                    Text = name.Length > 0 ? name : path, Style = (Style)res["RowTitleStyle"],
                    TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.NoWrap,
                },
                new TextBlock
                {
                    Text = path, Style = (Style)res["FootnoteStyle"], TextTrimming = TextTrimming.CharacterEllipsis, TextWrapping = TextWrapping.NoWrap,
                },
            },
        };
        ToolTipService.SetToolTip(text, path);
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        foreach (var (glyph, label, move, size) in new[] { ("\uE70E", "Move up", -1, 13), ("\uE70D", "Move down", 1, 13), ("\uE711", "Remove", 0, 12) })
        {
            var button = new Button
            {
                Style = (Style)res["IconButtonStyle"],
                Content = new FontIcon { Glyph = glyph, FontSize = size },
                IsEnabled = move == 0 || (index + move >= 0 && index + move < CurrentSettings.FolderStacks.Count),
            };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(button, Loc.T(label));
            ToolTipService.SetToolTip(button, Loc.T(label));
            button.Click += (_, _) => EditStack(index, move);
            buttons.Children.Add(button);
        }
        Grid.SetColumn(text, 1);
        Grid.SetColumn(buttons, 2);
        row.Children.Add(icon);
        row.Children.Add(text);
        row.Children.Add(buttons);
        return row;
    }

    // move: -1 up, 1 down, 0 remove.
    private void EditStack(int index, int move)
    {
        var folders = CurrentSettings.FolderStacks;
        if (index < 0 || index >= folders.Count) return;
        if (move == 0) folders.RemoveAt(index);
        else if (index + move >= 0 && index + move < folders.Count) (folders[index], folders[index + move]) = (folders[index + move], folders[index]);
        FillStackList();
        Changed();
    }

    private void AddStackFolder(string path)
    {
        var folders = CurrentSettings.FolderStacks;
        bool known = folders.Any(f => string.Equals(f.TrimEnd('\\'), path.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase));
        if (path.Length == 0 || folders.Count >= 32 || known) return;
        folders.Add(path);
        if (!CurrentSettings.ShowFolderStacks)
        {
            CurrentSettings.ShowFolderStacks = true; // a folder added is a stack wanted
            LoadExtras();
        }
        FillStackList();
        Changed();
    }

    private async Task AddStackFolderAsync()
    {
        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(this));
        picker.FileTypeFilter.Add("*");
        try
        {
            if (await picker.PickSingleFolderAsync() is { } folder) AddStackFolder(folder.Path);
        }
        catch (COMException)
        {
            ShowMessage(InfoBarSeverity.Warning, Loc.T("The file picker is unavailable"),
                Loc.T("Windows does not show it to apps run as administrator. Open the settings without administrator rights."));
        }
    }
}
