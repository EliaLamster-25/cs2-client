using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace Crymore.Loader.Controls;

public partial class RingProgress : UserControl
{
    public static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(
            nameof(Progress),
            typeof(double),
            typeof(RingProgress),
            new PropertyMetadata(double.NaN, OnProgressChanged));

    private Storyboard? _spinStoryboard;

    /// <summary>Ellipse stroke path length for the 12px ring (approx. pi * diameter).</summary>
    private const double Circumference = Math.PI * 11.0;

    public RingProgress()
    {
        InitializeComponent();
    }

    /// <summary>0–1 for determinate arc; NaN for indeterminate spin.</summary>
    public double Progress
    {
        get => (double)GetValue(ProgressProperty);
        set => SetValue(ProgressProperty, value);
    }

    private static void OnProgressChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is RingProgress ring)
            ring.UpdateVisual();
    }

    private void UpdateVisual()
    {
        _spinStoryboard?.Stop();
        _spinStoryboard = null;
        SpinTransform.Angle = 0;

        if (double.IsNaN(Progress) || Progress < 0)
        {
            // Single smooth arc segment that rotates (not a dashed ring).
            const double arcLen = Circumference * 0.28;
            ArcEllipse.StrokeDashArray = new DoubleCollection { arcLen, Circumference - arcLen };
            ArcEllipse.Opacity = 1;

            _spinStoryboard = new Storyboard();
            var anim = new DoubleAnimation(0, 360, TimeSpan.FromMilliseconds(850))
            {
                RepeatBehavior = RepeatBehavior.Forever,
            };
            Storyboard.SetTarget(anim, SpinTransform);
            Storyboard.SetTargetProperty(anim, new PropertyPath(RotateTransform.AngleProperty));
            _spinStoryboard.Children.Add(anim);
            _spinStoryboard.Begin();
            return;
        }

        var filled = Math.Clamp(Progress, 0, 1) * Circumference;
        var gap = Math.Max(0.01, Circumference - filled);
        ArcEllipse.StrokeDashArray = new DoubleCollection { filled, gap };
        ArcEllipse.Opacity = Progress > 0.001 ? 1 : 0.35;
    }
}
