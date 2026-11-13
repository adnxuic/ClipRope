using System;
using System.IO;
using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;

// Two non-native editing surfaces share one input HWND. WPF performs the paste.
sealed class SampleField : TextBox
{
    public int Shortcuts, Pastes;
    public SampleField()
    {
        Height = 48;
        DataObject.AddPastingHandler(this, delegate { Pastes++; });
    }
    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        if (e.Key == Key.V && Keyboard.Modifiers == ModifierKeys.Control) Shortcuts++;
        base.OnPreviewKeyDown(e);
    }
    protected override AutomationPeer OnCreateAutomationPeer() { return new GroupPeer(this); }
    sealed class GroupPeer : FrameworkElementAutomationPeer, IValueProvider
    {
        public GroupPeer(SampleField owner) : base(owner) {}
        protected override AutomationControlType GetAutomationControlTypeCore() { return AutomationControlType.Group; }
        public override object GetPattern(PatternInterface p) { return p == PatternInterface.Value ? this : base.GetPattern(p); }
        public bool IsReadOnly { get { return false; } }
        public string Value { get { return ((SampleField)Owner).Text; } }
        public void SetValue(string value) { throw new InvalidOperationException(); }
    }
}

static class Receiver
{
    [STAThread]
    static void Main(string[] args)
    {
        var a = new SampleField();
        var b = new SampleField();
        var panel = new StackPanel();
        panel.Children.Add(a);
        panel.Children.Add(b);
        var window = new Window { Title = "ClipRope paste receiver", Content = panel, Width = 320, Height = 160 };
        window.SourceInitialized += delegate {
            HwndSource.FromHwnd(new WindowInteropHelper(window).Handle).AddHook(
                delegate(IntPtr hwnd, int msg, IntPtr w, IntPtr l, ref bool handled) {
                    if (msg == 0x8001) {
                        a.Text = "\u5de6X\u53f3"; b.Text = "other";
                        a.Focus(); a.Select(1, 1);
                        a.Shortcuts = b.Shortcuts = a.Pastes = b.Pastes = 0;
                        handled = true;
                        return new IntPtr(a.IsKeyboardFocused ? 1 : 0);
                    }
                    if (msg == 0x8002) {
                        File.WriteAllLines(args[0], new[]{a.Text, b.Text, a.Shortcuts.ToString(),
                            b.Shortcuts.ToString(), a.Pastes.ToString(), b.Pastes.ToString()});
                        handled = true;
                    }
                    return IntPtr.Zero;
                });
        };
        new Application().Run(window);
    }
}
