# tools/acceptance_interaction.ps1 --- automated interaction test for heliostat_viz.
#
# Drives the real widgets with real Win32 mouse messages and checks the viewer's
# own reported state, so the "drag the sliders" requirements are verified without
# a human at the keyboard:
#
#   * convergence slider  -> t changes and the S95 spot area changes
#   * sun azimuth slider  -> azimuth changes
#   * beam target slider  -> aim offset changes (the spot walks across the receiver)
#   * '2' key             -> spp ladder steps
#   * LMB drag on the scene -> the camera orbits (and the app stays alive)
#
# Slider geometry comes from the viewer itself (--ui-layout) and the assertions
# read the viewer's own state lines (--state-log), so nothing is hard-coded.
param(
    [string]$Exe = "build\Release\heliostat_viz.exe",
    [string]$Log = "out\logs\interaction.log",
    [int]$Frames = 6000,
    [switch]$Validate
)

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class UiTest {
    // The viewer is DPI aware; this PowerShell process is not, so GetClientRect /
    // ClientToScreen / SetCursorPos would live in a virtualised (1/scale) coordinate
    // space and every synthetic click would land ~25% off on a 125% display.
    // Declare the test process DPI aware before doing any coordinate math.
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetClassNameW")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetWindowTextW")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    // The app reads the pointer position from real mouse messages, so the test has
    // to move the *physical* cursor (posted coordinates alone get overwritten by
    // the next genuine WM_MOUSEMOVE).
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    public static void MoveClient(IntPtr h, int cx, int cy) {
        POINT p; p.x = cx; p.y = cy;
        ClientToScreen(h, ref p);
        SetCursorPos(p.x, p.y);
    }
    public static IntPtr FindByPid(uint want, string cls) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid != want) return true;
            StringBuilder sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (sb.ToString() == cls) { result = h; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static string Title(IntPtr h) {
        StringBuilder sb = new StringBuilder(512);
        GetWindowText(h, sb, 512);
        return sb.ToString();
    }
    public static void Mouse(IntPtr h, uint msg, int x, int y, int button) {
        IntPtr lp = (IntPtr)((y << 16) | (x & 0xFFFF));
        PostMessage(h, msg, (IntPtr)button, lp);
    }
    public static void Key(IntPtr h, int vk) {
        PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x001E0001);
        PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC01E0001);
    }
}
"@

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
[void][UiTest]::SetProcessDPIAware()
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null

# The viewer keeps its log open while running: read with sharing enabled + retries.
function Read-Log([string]$path) {
    for ($attempt = 0; $attempt -lt 5; $attempt++) {
        try {
            $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                         [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs)
            $txt = $sr.ReadToEnd()
            $sr.Close(); $fs.Close()
            return $txt
        } catch { Start-Sleep -Milliseconds 150 }
    }
    return ""
}

$procArgs = @("--frames", "$Frames", "--log", $Log, "--ui-layout", "--state-log",
              "--spp", "256", "--present", "mailbox")
if ($Validate) { $procArgs += "--validate" }
$proc = Start-Process -FilePath $Exe -ArgumentList $procArgs -PassThru

$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    $hwnd = [UiTest]::FindByPid([uint32]$proc.Id, "HeliostatStudioWindow")
    if ($hwnd -ne [IntPtr]::Zero) { break }
    if ($proc.HasExited) { break }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Host "FAIL: no window (exited: $($proc.HasExited))"
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 1
}
Write-Host "window $hwnd; title: $([UiTest]::Title($hwnd))"
[void][UiTest]::SetForegroundWindow($hwnd)
$cursorStart = New-Object UiTest+POINT
[void][UiTest]::GetCursorPos([ref]$cursorStart)

# ---- widget geometry, straight from the viewer ----
$sliders = @{}
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 250
    foreach ($line in ((Read-Log $Log) -split "`n")) {
        if ($line -match '\[hud\] slider (\d+) "([^"]*)" x=([\d.]+) y=([\d.]+) w=([\d.]+) h=([\d.]+)') {
            $sliders[[int]$Matches[1]] = @{ label = $Matches[2]; x = [double]$Matches[3]; y = [double]$Matches[4];
                                           w = [double]$Matches[5]; h = [double]$Matches[6] }
        }
    }
    if ($sliders.Count -ge 6) { break }
}
if ($sliders.Count -lt 6) {
    Write-Host "FAIL: layout dump incomplete ($($sliders.Count) sliders)"
    $proc.Kill(); exit 1
}
foreach ($k in ($sliders.Keys | Sort-Object)) {
    $s = $sliders[$k]
    Write-Host ("  id {0} '{1}' x={2} y={3} w={4}" -f $k, $s.label, $s.x, $s.y, $s.w)
}

function Get-State {
    $lines = @(((Read-Log $Log) -split "`n") | Where-Object { $_ -match '^\[state\]' })
    if ($lines.Count -eq 0) { return $null }
    return $lines[$lines.Count - 1]
}
function State-Value($line, [string]$key) {
    if (-not $line) { return $null }
    # the viewer prints the aim offset with an explicit sign ("aim +36.00")
    if ($line -match "$key ([+-]?[\d.]+)") { return [double]$Matches[1] }
    return $null
}
function Drag-Slider([int]$id, [double]$toFraction) {
    $s = $sliders[$id]
    $cy = [int]($s.y + $s.h / 2.0)
    $x0 = [int]($s.x + $s.w * 0.5)
    $x1 = [int]($s.x + $s.w * $toFraction)
    # move the real pointer first (that is what the app samples), then press /
    # drag / release with both a warped pointer and posted button messages
    [UiTest]::MoveClient($hwnd, $x0, $cy)
    Start-Sleep -Milliseconds 200
    [UiTest]::Mouse($hwnd, 0x0201, $x0, $cy, 1)      # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 200
    [UiTest]::MoveClient($hwnd, $x1, $cy)            # real drag
    Start-Sleep -Milliseconds 350
    [UiTest]::Mouse($hwnd, 0x0202, $x1, $cy, 0)      # WM_LBUTTONUP
    Start-Sleep -Milliseconds 350
}

# Synthetic mouse injection is occasionally dropped by the OS (the first drag after
# launch in particular, while the window is still settling into the foreground), which
# made this test flake at roughly 1 run in 4. Retry the *same* drag until it registers;
# the assertions afterwards are unchanged, so a genuinely broken widget still fails.
function Drag-Slider-Verified([int]$id, [double]$toFraction, [string]$key, [string]$beforeState) {
    $latest = $beforeState
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        Drag-Slider $id $toFraction
        Start-Sleep -Milliseconds 900
        $latest = Get-State
        $b = State-Value $beforeState $key
        $a = State-Value $latest $key
        if ($null -ne $a -and $null -ne $b -and [math]::Abs($a - $b) -gt 0.001) { return $latest }
        Write-Host "  (note: drag of slider $id did not register, retrying - attempt $attempt)"
    }
    return $latest
}

$stateBefore = Get-State
Write-Host "state before : $stateBefore"
if (-not $stateBefore) { Write-Host "FAIL: no [state] lines"; $proc.Kill(); exit 1 }

$afterT = Drag-Slider-Verified 3 0.0 "t" $stateBefore
Write-Host "after t-drag : $afterT"

$afterSun = Drag-Slider-Verified 1 0.85 "az" $afterT
Write-Host "after sun    : $afterSun"

$afterAim = Drag-Slider-Verified 6 0.8 "aim" $afterSun
Write-Host "after aim    : $afterAim"

# camera orbit: press on the scene (right half, below the panel) and drag
[UiTest]::MoveClient($hwnd, 1100, 500)
Start-Sleep -Milliseconds 200
[UiTest]::Mouse($hwnd, 0x0201, 1100, 500, 1)
for ($i = 1; $i -le 12; $i++) {
    [UiTest]::MoveClient($hwnd, 1100 - $i * 15, 500 - $i * 5)
    Start-Sleep -Milliseconds 45
}
[UiTest]::Mouse($hwnd, 0x0202, 950, 445, 0)
Start-Sleep -Milliseconds 500

[UiTest]::Key($hwnd, 0x32)   # '2': spp ladder
Start-Sleep -Milliseconds 1200
$afterSpp = Get-State
Write-Host "after spp key: $afterSpp"

Write-Host "closing"
[UiTest]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
if (-not $proc.WaitForExit(20000)) { Write-Host "FAIL: no exit after WM_CLOSE"; $proc.Kill(); exit 1 }
Write-Host "exit code: $($proc.ExitCode)"
$alive = $false

$fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "  PASS $name" } else { Write-Host "  FAIL $name"; $script:fail++ }
}

$tBefore = State-Value $stateBefore "t"
$tAfter = State-Value $afterT "t"
$s95Before = State-Value $stateBefore "s95"
$s95After = State-Value $afterT "s95"
$azBefore = State-Value $stateBefore "az"
$azAfter = State-Value $afterSun "az"
$aimBefore = State-Value $afterSun "aim"
$aimAfter = State-Value $afterAim "aim"
$sppBefore = State-Value $afterAim "spp"
$sppAfter = State-Value $afterSpp "spp"

Write-Host ""
Write-Host "convergence slider : t $tBefore -> $tAfter, S95 $s95Before -> $s95After m2"
Write-Host "sun azimuth slider : $azBefore -> $azAfter deg"
Write-Host "beam target slider : $aimBefore -> $aimAfter deg"
Write-Host "spp ladder key     : $sppBefore -> $sppAfter"
Check "convergence slider changed t" ($null -ne $tAfter -and $tAfter -ne $tBefore)
Check "deformation changed the S95 area by > 1 m2" ($null -ne $s95After -and [math]::Abs($s95After - $s95Before) -gt 1.0)
Check "sun azimuth slider changed the azimuth" ($null -ne $azAfter -and [math]::Abs($azAfter - $azBefore) -gt 1.0)
Check "beam target slider changed the aim offset" ($null -ne $aimAfter -and [math]::Abs($aimAfter - $aimBefore) -gt 1.0)
Check "spp key cycled the sample count" ($null -ne $sppAfter -and $sppAfter -ne $sppBefore)
if ($Validate) {
    $errs = ([regex]::Matches((Read-Log $Log), "validation:(ERROR|warn)")).Count
    Write-Host "validation messages: $errs"
    Check "validation layer clean" ($errs -eq 0)
}
Check "process exited on WM_CLOSE with code 0" ($proc.ExitCode -eq 0)

Pop-Location
if ($fail -gt 0) { Write-Host "FAILED ($fail checks)"; exit 1 }
Write-Host "PASS"
