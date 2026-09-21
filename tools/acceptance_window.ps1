# tools/window_test.ps1 — automated window acceptance test for heliostat_viz.
#
# Drives the window from the outside (no human needed): resize a few times,
# minimise/restore/maximise, toggle a few A/B keys by posting WM_KEYDOWN, then
# close the window and check the log for validation errors.
#
#   powershell -File tools\window_test.ps1 -Exe build\Release\heliostat_viz.exe
param(
    [string]$Exe = "build\Release\heliostat_viz.exe",
    [string]$Log = "out\window_test.log",
    [string]$Class = "HeliostatStudioWindow",
    [int]$Frames = 1500,
    [switch]$Validate
)

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Test {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }

    // FindWindow() proves unreliable here, so locate the window by owning process.
    public static IntPtr FindByPid(uint want, string cls) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, l) => {
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != want) { return true; }
            StringBuilder sb = new StringBuilder(256);
            GetClassName(h, sb, 256);
            if (sb.ToString() == cls) { result = h; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
"@

$procArgs = @("--frames", "$Frames", "--log", $Log)
if ($Validate) { $procArgs += "--validate" }
$proc = Start-Process -FilePath $Exe -ArgumentList $procArgs -PassThru

# The window appears a little later than the process (device + pipeline creation,
# slower under the validation layer), so poll for it.
$hwnd = [IntPtr]::Zero
$tries = 0
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    $tries = $i
    $hwnd = [Win32Test]::FindByPid([uint32]$proc.Id, $Class)
    if ($hwnd -ne [IntPtr]::Zero) { break }
    if ($proc.HasExited) { break }
}
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Host "FAIL: window class '$Class' not found (process exited: $($proc.HasExited))"
    if (-not $proc.HasExited) { $proc.Kill() }
    exit 1
}
Write-Host "window found: $hwnd after $($tries * 250) ms"

function ClientSize($h) {
    $r = New-Object Win32Test+RECT
    [void][Win32Test]::GetClientRect($h, [ref]$r)
    return "$($r.right)x$($r.bottom)"
}

$sizes = @(@(1600, 900), @(1024, 600), @(640, 480), @(320, 240), @(1280, 720))
foreach ($s in $sizes) {
    [void][Win32Test]::SetWindowPos($hwnd, [IntPtr]::Zero, 40, 40, $s[0], $s[1], 0x0004)  # SWP_NOZORDER
    Start-Sleep -Milliseconds 400
    Write-Host ("  resize -> requested {0}x{1}, client {2}, alive {3}" -f $s[0], $s[1], (ClientSize $hwnd), [Win32Test]::IsWindow($hwnd))
}

Write-Host "  minimise"; [void][Win32Test]::ShowWindow($hwnd, 6)   # SW_MINIMIZE
Start-Sleep -Milliseconds 900
Write-Host "  restore";  [void][Win32Test]::ShowWindow($hwnd, 9)   # SW_RESTORE
Start-Sleep -Milliseconds 600
Write-Host "  maximise"; [void][Win32Test]::ShowWindow($hwnd, 3)   # SW_MAXIMIZE
Start-Sleep -Milliseconds 800
Write-Host "  restore";  [void][Win32Test]::ShowWindow($hwnd, 9)
Start-Sleep -Milliseconds 500

# A/B switches: 1 = cull, 2 = spp ladder, 3 = atomics, F6/F7/F8 = view modes.
$keys = @(0x31, 0x32, 0x32, 0x32, 0x33, 0x75, 0x76, 0x77)
foreach ($k in $keys) {
    [void][Win32Test]::PostMessage($hwnd, 0x0100, [IntPtr]$k, [IntPtr]0)  # WM_KEYDOWN
    [void][Win32Test]::PostMessage($hwnd, 0x0101, [IntPtr]$k, [IntPtr]0)  # WM_KEYUP
    Start-Sleep -Milliseconds 150
}
Write-Host "  key events posted, alive $([Win32Test]::IsWindow($hwnd))"

Start-Sleep -Milliseconds 800
Write-Host "  posting WM_CLOSE"
[void][Win32Test]::PostMessage($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE
if (-not $proc.WaitForExit(15000)) {
    Write-Host "FAIL: process did not exit after WM_CLOSE"
    $proc.Kill()
    exit 1
}
Write-Host "exit code: $($proc.ExitCode)"

if (Test-Path $Log) {
    $txt = Get-Content $Log -Raw
    $resizes = ([regex]::Matches($txt, "\[viz\] resize ->")).Count
    Write-Host "resize events logged: $resizes"
    $errs = ([regex]::Matches($txt, "validation:(ERROR|warn)")).Count
    Write-Host "validation errors/warnings: $errs"
    if ($errs -gt 0) {
        Get-Content $Log | Select-String -Pattern "validation:" | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    }
    Get-Content $Log | Select-Object -Last 5
    if ($resizes -lt 3) { Write-Host "FAIL: expected at least 3 resize events"; exit 1 }
    if ($errs -gt 0) { Write-Host "FAIL: validation layer reported errors"; exit 1 }
}
Write-Host "PASS"
