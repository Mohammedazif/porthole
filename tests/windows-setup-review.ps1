param(
    [Parameter(Mandatory)] [string] $Installer,
    [Parameter(Mandatory)] [string] $OutputDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SetupReview {
    public delegate bool ChildVisitor(IntPtr window, IntPtr context);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string caption);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr window, ChildVisitor visitor, IntPtr context);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr param, IntPtr value);
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr window);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    public static IntPtr FindVisible(IntPtr parent, string className, string caption) {
        IntPtr match = IntPtr.Zero;
        EnumChildWindows(parent, (window, _) => {
            if (!IsWindowVisible(window)) return true;
            var text = new StringBuilder(256);
            GetClassName(window, text, text.Capacity);
            if (text.ToString() != className) return true;
            GetWindowText(window, text, text.Capacity);
            if (caption != null && !text.ToString().Replace("&","").StartsWith(caption))
                return true;
            match = window;
            return false;
        }, IntPtr.Zero);
        return match;
    }
}
'@

function Save-WizardImage([IntPtr] $Window, [string] $File) {
    $rect = [SetupReview+Rect]::new()
    if (![SetupReview]::GetWindowRect($Window, [ref]$rect)) { throw 'Cannot measure Setup' }
    $bitmap = [System.Drawing.Bitmap]::new(($rect.Right - $rect.Left), ($rect.Bottom - $rect.Top))
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try {
            if (![SetupReview]::PrintWindow($Window, $dc, 2)) { throw 'Cannot capture Setup' }
        } finally {
            $graphics.ReleaseHdc($dc)
        }
        $bitmap.Save($File, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

$OutputDirectory = (New-Item -ItemType Directory -Force $OutputDirectory).FullName
$process = Start-Process -FilePath (Resolve-Path $Installer).Path -ArgumentList '/NORESTART' -PassThru
[uint32] $wizardProcess = 0
try {
    $wizard = [IntPtr]::Zero
    for ($attempt = 0; $attempt -lt 100 -and $wizard -eq [IntPtr]::Zero; $attempt++) {
        Start-Sleep -Milliseconds 100
        $wizard = [SetupReview]::FindWindow('TWizardForm', 'Setup - Porthole')
    }
    if ($wizard -eq [IntPtr]::Zero) { throw 'The Windows runner did not expose the Setup window' }
    [void][SetupReview]::GetWindowThreadProcessId($wizard, [ref]$wizardProcess)
    Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-destination.png')
    [void][SetupReview]::SetForegroundWindow($wizard)
    for ($page = 0; $page -lt 3; $page++) {
        $options = [SetupReview]::FindVisible($wizard, 'TNewCheckListBox', $null)
        if ($options -ne [IntPtr]::Zero) {
            Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-options.png')
            return
        }
        $next = [SetupReview]::FindVisible($wizard, 'TNewButton', 'Next')
        if ($next -eq [IntPtr]::Zero) { throw 'Setup could not advance to Installation options' }
        [void][SetupReview]::PostMessage($next, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
        Start-Sleep -Milliseconds 650
    }
    throw 'Setup did not display Installation options'
} finally {
    if ($wizardProcess -ne 0 -and $wizardProcess -ne $process.Id) {
        Stop-Process -Id $wizardProcess -Force -ErrorAction SilentlyContinue
    }
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
}
