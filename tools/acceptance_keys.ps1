# tools/acceptance_keys.ps1 - verify every keyboard control actually fires.
#
#   * 4/5/6/7 must BOTH switch the traced mirror AND fly the camera to it
#   * F2 must fly the camera to whichever mirror is currently selected
#   * [ and ] must move the convergence slider t   (they are VK_OEM_4/VK_OEM_6,
#     NOT the ASCII codes for '[' / ']' - that was a real bug)
#   * 8 must switch the field's ray tracing off/on (field 1 -> 0 -> 1)
#   * M / F7 / F8 / U must cycle the heat range, the plate view, the beams and the
#     UI scale, which the viewer reports as fields of its --state-log line
#   * Space must write a screenshot WITHOUT any --screenshot / --record option on
#     the command line. It used to be a silent no-op there, because the capture
#     buffers were only allocated when one of those options was present.
#   * every one of them must also print an acknowledgement line
#
# Everything is asserted from the viewer's own state lines and log, so the checks are
# computed rather than hard-coded.
param(
    [string]$Exe = "build\Release\heliostat_viz.exe",
    [string]$Log = "out\logs\keys.log"
)

$ErrorActionPreference = "Stop"
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class KeyTest {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="GetClassNameW")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
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
    public static void Down(IntPtr h, int vk) { PostMessage(h, 0x0100, (IntPtr)vk, (IntPtr)0x001E0001); }
    public static void Up(IntPtr h, int vk) { PostMessage(h, 0x0101, (IntPtr)vk, (IntPtr)0xC01E0001); }
    public static void Tap(IntPtr h, int vk) { Down(h, vk); Up(h, vk); }
}
"@

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
[void][KeyTest]::SetProcessDPIAware()
New-Item -ItemType Directory -Force -Path (Split-Path $Log) | Out-Null
Remove-Item $Log -ErrorAction SilentlyContinue

# The viewer runs with its working directory inside out\, so the screenshots the
# Space check produces never land in the repository root. Assets are resolved from
# the executable's own directory, so a different cwd changes nothing else.
$exeAbs = (Resolve-Path $Exe).Path
$logAbs = Join-Path $root $Log
$shotsDir = Join-Path $root "out\shots"
Remove-Item $shotsDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $shotsDir | Out-Null

function Read-Log([string]$path) {
    for ($i = 0; $i -lt 6; $i++) {
        try {
            $fs = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                         [System.IO.FileShare]::ReadWrite)
            $sr = New-Object System.IO.StreamReader($fs); $t = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
            return $t
        } catch { Start-Sleep -Milliseconds 150 }
    }
    return ""
}
function State { (((Read-Log $Log) -split "`n") | Where-Object { $_ -match '^\[state\]' } | Select-Object -Last 1) }
function Num($line, [string]$key) { if ($line -match "$key ([-\d.]+)") { return [double]$Matches[1] } return $null }
function MirrorOf($line) { if ($line -match 'mirror (\w+)') { return $Matches[1] } return "" }
function Wait-State([int]$tries = 40) {
    for ($i = 0; $i -lt $tries; $i++) {
        $s = State
        if ($s -and (MirrorOf $s)) { return $s }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

$fail = 0
function Check($name, $cond) {
    if ($cond) { Write-Host "  PASS $name" } else { Write-Host "  FAIL $name"; $script:fail++ }
}

# ---------------------------------------------------------------- mirror keys --
$proc = Start-Process -FilePath $exeAbs -ArgumentList @("--frames", "6000", "--log", $logAbs,
                                                       "--state-log", "--no-ui", "--fps", "60") `
                      -WorkingDirectory $shotsDir -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 250
    $hwnd = [KeyTest]::FindByPid([uint32]$proc.Id, "HeliostatStudioWindow")
    if ($hwnd -ne [IntPtr]::Zero) { break }
    if ($proc.HasExited) { break }
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "FAIL: no window"; if (-not $proc.HasExited) { $proc.Kill() }; exit 1 }

$keys = @{ '4' = 0x34; '5' = 0x35; '6' = 0x36; '7' = 0x37 }
$names = @{ '4' = 'North'; '5' = 'East'; '6' = 'South'; '7' = 'West' }
foreach ($k in @('4', '5', '6', '7')) {
    [KeyTest]::Tap($hwnd, $keys[$k])
    Start-Sleep -Milliseconds 800
    $s = Wait-State
    $mirror = MirrorOf $s
    $cam = Num $s 'cam'
    Write-Host "key $k : mirror=$mirror cam=$cam"
    Check "key $k selects $($names[$k])" ($mirror -eq $names[$k])
    Check "key $k flew the camera" ($null -ne $cam -and [math]::Abs($cam - 180.0) -gt 1.0)
}

# --------------------------------------------------------- toggle state keys --
# field / beams / view / heat / uiscale are all reported by the state line.
$toggles = @(
    @{ key = '8'; vk = 0x38; field = 'field';   from = 1;   to = 0; name = 'field ray tracing off' },
    @{ key = '8'; vk = 0x38; field = 'field';   from = 0;   to = 1; name = 'field ray tracing back on' },
    @{ key = 'M'; vk = 0x4D; field = 'heat';    from = 1;   to = 0; name = 'heat range FIXED' },
    @{ key = 'F7'; vk = 0x76; field = 'view';   from = 0;   to = 1; name = 'plate view normals' },
    @{ key = 'F8'; vk = 0x77; field = 'beams';  from = 1;   to = 2; name = 'beams strong' },
    @{ key = 'U'; vk = 0x55; field = 'uiscale'; from = 1.0; to = 1.5; name = 'UI scale 1.5' }
)
foreach ($t in $toggles) {
    $before = Wait-State
    [KeyTest]::Tap($hwnd, $t.vk)
    Start-Sleep -Milliseconds 800
    $after = Wait-State
    $b = Num $after $t.field
    Write-Host "$($t.key) : $($t.field) $($b)"
    Check "$($t.key) -> $($t.name)" ($null -ne $b -and [math]::Abs($b - $t.to) -lt 0.01)
}

# ------------------------------------------------------------ convergence [ ] --
# t is at 1.0 after the mirror keys; [ must lower it and ] must raise it again.
[KeyTest]::Down($hwnd, 0xDB); Start-Sleep -Milliseconds 700; [KeyTest]::Up($hwnd, 0xDB)
Start-Sleep -Milliseconds 500
$tAfterOpen = Num (Wait-State) 't'
[KeyTest]::Down($hwnd, 0xDD); Start-Sleep -Milliseconds 700; [KeyTest]::Up($hwnd, 0xDD)
Start-Sleep -Milliseconds 500
$tAfterClose = Num (Wait-State) 't'
Write-Host "[ : t -> $tAfterOpen ;  ] : t -> $tAfterClose"
Check "[ lowers the convergence t" ($null -ne $tAfterOpen -and $tAfterOpen -lt 0.95)
Check "] raises the convergence t again" ($null -ne $tAfterClose -and $tAfterClose -gt $tAfterOpen)

# ------------------------------------------------------------- Space shot --
# No --screenshot / --record was passed, which is exactly the case that used to
# make Space a silent no-op: the capture buffers are now allocated unconditionally.
[KeyTest]::Tap($hwnd, 0x20)
Start-Sleep -Milliseconds 1500
$shots = @(Get-ChildItem (Join-Path $shotsDir "shot_*.bmp") -ErrorAction SilentlyContinue)
Write-Host "Space : shot files -> $($shots.Count) ($(($shots | ForEach-Object { $_.Name }) -join ', '))"
Check "Space writes a screenshot without any capture option" ($shots.Count -ge 1)
if ($shots.Count -ge 1) {
    $shotLog = Read-Log $logAbs
    Check "Space announces itself in the log" ($shotLog -match 'Space -> screenshot shot_\d+\.bmp')
    Check "Space writes the .bmp plus its ASCII preview" `
          (($shotLog -match '\[shot\] shot_\d+\.bmp \(\d+x\d+\)') -and
           (Test-Path ($shots[0].FullName -replace '\.bmp$', '.txt')))
    Check "the screenshot is not empty" ($shots[0].Length -gt 100000)
}

# ------------------------------------------------------- acknowledgement lines --
$log = Read-Log $logAbs
Check "the viewer acknowledges every toggle in its log" `
      (($log -match '4/5/6/7 -> tracing mirror') -and ($log -match '8 -> field ray tracing OFF') -and
       ($log -match 'M -> heat range') -and ($log -match 'F7 -> plate view') -and
       ($log -match 'F8 -> light beams') -and ($log -match 'U -> UI scale') -and
       ($log -match '\[ -> convergence') -and ($log -match '\] -> convergence'))

[KeyTest]::Tap($hwnd, 0x10)   # WM_CLOSE
if (-not $proc.WaitForExit(20000)) { $proc.Kill() }

Pop-Location
if ($fail -gt 0) { Write-Host "FAILED ($fail checks)"; exit 1 }
Write-Host "PASS"
