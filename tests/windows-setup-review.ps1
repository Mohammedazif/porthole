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
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr capacity, StringBuilder text);
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

    public static IntPtr FindWizard() {
        IntPtr window = FindWindow("TWizardForm", null);
        if (window == IntPtr.Zero) return window;
        var title = new StringBuilder(256);
        GetWindowText(window, title, title.Capacity);
        return title.ToString().StartsWith("Setup - Porthole") ? window : IntPtr.Zero;
    }

    public static IntPtr FindVisible(IntPtr parent, string className, string caption) {
        IntPtr match = IntPtr.Zero;
        EnumChildWindows(parent, (window, _) => {
            if (!IsWindowVisible(window)) return true;
            var text = new StringBuilder(256);
            GetClassName(window, text, text.Capacity);
            if (text.ToString() != className) return true;
            if (caption != null) {
                SendMessage(window, 0x000D, (IntPtr)text.Capacity, text);
                if (!text.ToString().Replace("&","").StartsWith(caption)) return true;
            }
            match = window;
            return false;
        }, IntPtr.Zero);
        return match;
    }

    public static string ReadText(IntPtr window) {
        var text = new StringBuilder(4096);
        SendMessage(window, 0x000D, (IntPtr)text.Capacity, text);
        return text.ToString();
    }

    public static bool Click(IntPtr button) {
        return PostMessage(button, 0x00F5, IntPtr.Zero, IntPtr.Zero);
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
        $wizard = [SetupReview]::FindWizard()
    }
    if ($wizard -eq [IntPtr]::Zero) { throw 'The Windows runner did not expose the Setup window' }
    [void][SetupReview]::GetWindowThreadProcessId($wizard, [ref]$wizardProcess)
    Start-Sleep -Milliseconds 300
    Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-destination.png')
    [void][SetupReview]::SetForegroundWindow($wizard)
    for ($page = 0; $page -lt 3; $page++) {
        $network = [SetupReview]::FindVisible($wizard, 'TBitmapButton', 'Allow connections on private networks')
        if ($network -ne [IntPtr]::Zero) {
            $startup = [SetupReview]::FindVisible($wizard, 'TBitmapButton', 'Start Porthole at sign-in for all users')
            if ($startup -eq [IntPtr]::Zero) { throw 'The startup switch is missing' }
            if (![SetupReview]::ReadText($network).EndsWith(': On') -or
                ![SetupReview]::ReadText($startup).EndsWith(': Off')) {
                throw 'Incorrect initial installer options'
            }
            Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-options.png')
            if (![SetupReview]::Click($network) -or ![SetupReview]::Click($startup)) {
                throw 'Could not toggle the installer options'
            }
            Start-Sleep -Milliseconds 300
            if (![SetupReview]::ReadText($network).EndsWith(': Off') -or
                ![SetupReview]::ReadText($startup).EndsWith(': On')) {
                throw 'The installer switches did not update'
            }
            Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-options-selected.png')
            $next = [SetupReview]::FindVisible($wizard, 'TNewButton', 'Next')
            if ($next -eq [IntPtr]::Zero) { throw 'Cannot review the selected options' }
            [void][SetupReview]::PostMessage($next, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
            Start-Sleep -Milliseconds 650
            $ready = [SetupReview]::FindVisible($wizard, 'TNewMemo', $null)
            if ($ready -eq [IntPtr]::Zero) { throw 'Cannot read the install summary' }
            $summary = [SetupReview]::ReadText($ready)
            if ($summary -match 'Allow connections on private networks' -or
                $summary -notmatch 'Start Porthole at sign-in for all users') {
                throw 'The install summary did not match the selected options'
            }
            Save-WizardImage $wizard (Join-Path $OutputDirectory 'setup-ready.png')
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
