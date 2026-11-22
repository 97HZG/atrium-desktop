using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;
using Path = Microsoft.UI.Xaml.Shapes.Path;

namespace Atrium.Settings;

// The three extra entries as BarExtrasPainter draws them: Launchpad's nine tiles, the now-playing
// entry's five bars (held mid-swing, as if a song played) and the folder stack's two folders.
internal sealed partial class BarFace
{
    // kTileColours: each tile from its lit top to its deeper bottom.
    private static readonly (uint Top, uint Bottom)[] TileColours =
    [
        (0xFF8A8A, 0xF03E5E), (0xFFB861, 0xF76707), (0xFFE066, 0xF5A700),
        (0xD0A2FF, 0x9333EA), (0x5CCBFF, 0x1A80F0), (0x7BE08A, 0x2F9E44),
        (0xFF9EC4, 0xD6336C), (0x7EE3F0, 0x1098AD), (0xB8D4FF, 0x5B7CFA),
    ];

    // MusicBars: the silhouette (kShape) over the shortest bar (kRest), here at 60 % of its reach.
    private static readonly double[] BarShape = [0.58, 0.86, 1, 0.80, 0.52];

    // DrawStacks in a unit square: the front folder with its tab, the edge of the one behind.
    private const string FrontFolder =
        "F0 M0,0.41 L0,0.93 Q0,1 0.07,1 L0.79,1 Q0.86,1 0.86,0.93 L0.86,0.49 Q0.86,0.42 0.79,0.42 L0.43,0.42 L0.34,0.26 L0.07,0.26 Q0,0.26 0,0.33 Z";
    private const string BackFolder = "F0 M0.30,0.12 L0.93,0.12 Q1,0.12 1,0.19 L1,0.84";

    private readonly Canvas _launchpad = new() { IsHitTestVisible = false }, _music = new() { IsHitTestVisible = false };
    private readonly Rectangle[] _tiles = new Rectangle[9], _bars = new Rectangle[5];
    private readonly Path _folder = Folder(FrontFolder), _folderBack = Folder(BackFolder);

    private void AddExtras(Canvas canvas)
    {
        for (int i = 0; i < _tiles.Length; i++)
        {
            var g = new LinearGradientBrush { StartPoint = new Point(0.5, 0), EndPoint = new Point(0.5, 1) };
            g.GradientStops.Add(new GradientStop { Color = Rgb(TileColours[i].Top), Offset = 0 });
            g.GradientStops.Add(new GradientStop { Color = Rgb(TileColours[i].Bottom), Offset = 1 });
            _tiles[i] = new Rectangle { IsHitTestVisible = false, Fill = g };
            ContinuousCorners.SetEnabled(_tiles[i], true);
            _launchpad.Children.Add(_tiles[i]);
        }
        for (int i = 0; i < _bars.Length; i++)
        {
            _bars[i] = new Rectangle { IsHitTestVisible = false };
            _music.Children.Add(_bars[i]);
        }
        foreach (var e in new UIElement[] { _launchpad, _music, _folderBack, _folder })
            canvas.Children.Add(e);
    }

    private void LayoutExtras(MainWindow.BarGeometry l, Func<double, (double X, double Y, double Size)> item, Color ink)
    {
        _launchpad.Visibility = l.LaunchX < 0 ? Visibility.Collapsed : Visibility.Visible;
        if (l.LaunchX >= 0)
        {
            // 78 % of the icon square, gaps of a tenth, squircle corners a third of a tile.
            var (x, y, size) = item(l.LaunchX);
            double w = size * 0.78, gap = w * 0.10, tile = (w - 2 * gap) / 3;
            for (int i = 0; i < _tiles.Length; i++)
            {
                var t = _tiles[i];
                t.Width = t.Height = tile;
                t.RadiusX = t.RadiusY = tile * 0.32;
                Canvas.SetLeft(t, i % 3 * (tile + gap));
                Canvas.SetTop(t, i / 3 * (tile + gap));
            }
            Canvas.SetLeft(_launchpad, x + (size - w) / 2);
            Canvas.SetTop(_launchpad, y + (size - w) / 2);
        }

        _music.Visibility = l.MusicX < 0 ? Visibility.Collapsed : Visibility.Visible;
        if (l.MusicX >= 0)
        {
            var (x, y, size) = item(l.MusicX);
            double side = size * 0.80, bar = side * 0.11, gap = side * 0.0925;
            double left = x + size / 2 - (5 * bar + 4 * gap) / 2, cy = y + size / 2;
            var brush = new SolidColorBrush(ink);
            for (int i = 0; i < _bars.Length; i++)
            {
                double h = Math.Max(bar, side * (0.22 + 0.78 * 0.6 * BarShape[i]));
                var b = _bars[i];
                b.Width = bar;
                b.Height = h;
                b.RadiusX = b.RadiusY = bar / 2;
                b.Fill = brush;
                Canvas.SetLeft(b, left + i * (bar + gap));
                Canvas.SetTop(b, cy - h / 2);
            }
        }

        bool stacks = l.StacksX >= 0;
        _folder.Visibility = _folderBack.Visibility = stacks ? Visibility.Visible : Visibility.Collapsed;
        if (stacks)
        {
            // 74 % of the icon square, a stroke of 7.5 % of it; the folder behind at 55 % ink.
            var (x, y, size) = item(l.StacksX);
            double s = size * 0.74;
            foreach (var (p, alpha) in new[] { (_folder, 1.0), (_folderBack, 0.55) })
            {
                p.Stroke = new SolidColorBrush(Color.FromArgb((byte)(ink.A * alpha), ink.R, ink.G, ink.B));
                p.StrokeThickness = Math.Max(1, s * 0.075) / s;
                p.RenderTransform = new ScaleTransform { ScaleX = s, ScaleY = s };
                Canvas.SetLeft(p, x + (size - s) / 2);
                Canvas.SetTop(p, y + (size - s) / 2);
            }
        }
    }

    private static Path Folder(string markup) => new()
    {
        IsHitTestVisible = false,
        Data = (Geometry)XamlBindingHelper.ConvertValue(typeof(Geometry), markup),
        StrokeStartLineCap = PenLineCap.Round,
        StrokeEndLineCap = PenLineCap.Round,
        StrokeLineJoin = PenLineJoin.Round,
    };

    private static Color Rgb(uint rgb) => Color.FromArgb(255, (byte)(rgb >> 16), (byte)(rgb >> 8), (byte)rgb);
}
