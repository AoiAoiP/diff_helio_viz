# tools/make_stills.ps1 - export the portfolio stills + the perf plots.
#
#   powershell -File tools\make_stills.ps1
#
# Every still is a real frame: the viewer renders N frames, dumps the swapchain
# image to BMP, and ImageMagick converts it to PNG. Nothing is retouched.
param(
    [string]$Exe = "build\Release\heliostat_viz.exe",
    [string]$OutDir = "docs/figs"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
New-Item -ItemType Directory -Force -Path $OutDir, "out\logs", "out\stills" | Out-Null

$shots = @(
    @{ name = "still_1_overview";      args = @("--preset", "0", "--spp", "1024", "--t", "1", "--sun", "1") },
    @{ name = "still_2_spot_bloom";    args = @("--preset", "2", "--spp", "1024", "--t", "1", "--sun", "1") },
    @{ name = "still_3_deformation";   args = @("--preset", "1", "--spp", "512", "--t", "1", "--deform", "2", "--sun", "1") },
    @{ name = "still_4_normals";       args = @("--preset", "1", "--spp", "512", "--t", "0", "--debug-view", "1", "--sun", "1") },
    @{ name = "still_5_flat_plate";    args = @("--preset", "1", "--spp", "512", "--t", "0", "--sun", "1") },
    # --- field / orientation / sun-path features ---
    @{ name = "still_6_field_traced";  args = @("--preset", "0", "--spp", "1024", "--t", "1", "--mirror", "1", "--sun", "1") },
    @{ name = "still_7_sunpath_solstice"; args = @("--preset", "0", "--spp", "512", "--t", "1", "--sunpath", "1", "--sun-hour", "0") },
    @{ name = "still_8_south_spot";    args = @("--preset", "2", "--spp", "1024", "--t", "1", "--mirror", "2", "--sun", "1") }
)

foreach ($s in $shots) {
    $bmp = "out/stills/$($s.name).bmp"
    $log = "out/logs/$($s.name).txt"
    $pargs = @("--frames", "150", "--shot-frame", "120", "--screenshot", $bmp, "--log", $log,
               "--present", "mailbox", "--hud-scale", "1.3") + $s.args
    $p = Start-Process -FilePath $Exe -ArgumentList $pargs -PassThru
    $p.WaitForExit()
    if (-not (Test-Path $bmp)) { Write-Host "FAILED: $($s.name)"; continue }
    & magick $bmp "$OutDir/$($s.name).png"
    $mb = [math]::Round((Get-Item "$OutDir/$($s.name).png").Length / 1KB, 1)
    Write-Host ("  {0,-22} -> {1}/{0}.png ({2} KB)" -f $s.name, $OutDir, $mb)
}

Write-Host "performance plots"
python tools/plot_perf.py
Pop-Location
