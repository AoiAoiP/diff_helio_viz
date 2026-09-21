# tools/perf_sweep.ps1 — reproducible performance sweep for heliostat_viz.
#
# Runs the viewer over a parameter matrix and writes one CSV row per
# configuration, parsed from the viewer's own end-of-run report (GPU timestamp
# per pass + wall clock). Nothing is estimated: every number comes from a run.
#
#   powershell -File tools\perf_sweep.ps1 -Out docs/perf_viewer.csv
param(
    [string]$Exe = "build\Release\heliostat_viz.exe",
    [string]$Out = "docs/perf_viewer.csv",
    [int]$Frames = 400,
    [string]$Present = "immediate",
    [string]$Preset = "0",
    [switch]$Quick
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
New-Item -ItemType Directory -Force -Path "out\logs" | Out-Null

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add("case,spp,cull,atomics,rays_m,gpu_deform_ms,gpu_flux_ms,gpu_scene_ms,gpu_bloom_ms,gpu_post_ms,gpu_total_ms,wall_ms,fps,mray_per_s,s95_area_m2")

function Run-Case {
    param([string]$Name, [string]$Spp, [string[]]$ExtraArgs)
    if (-not $ExtraArgs) { $ExtraArgs = @() }
    $log = "out\logs\sweep_$Name.txt"
    $pargs = @("--frames", "$Frames", "--present", $Present, "--preset", $Preset,
               "--spp", $Spp, "--log", $log, "--no-ui") + $ExtraArgs
    $p = Start-Process -FilePath $Exe -ArgumentList $pargs -PassThru
    $p.WaitForExit()
    $txt = Get-Content $log -Raw
    if ($txt -match "deform ([\d.]+) \| flux ([\d.]+) \| scene ([\d.]+) \| bloom\.pre ([\d.]+) \| bloom\.down ([\d.]+) \| bloom\.up ([\d.]+) \| composite ([\d.]+) \| total ([\d.]+) ms") {
        $deform = $Matches[1]; $flux = $Matches[2]; $scene = $Matches[3]
        $bloom = [double]$Matches[4] + [double]$Matches[5] + [double]$Matches[6]
        $post = $Matches[7]; $total = $Matches[8]
    } else { Write-Host "  FAILED to parse GPU line for $Name"; return }
    if ($txt -match "wall avg ([\d.]+) ms \(([\d.]+) FPS\)") { $wall = $Matches[1]; $fps = $Matches[2] }
    else { $wall = "0"; $fps = "0" }
    if ($txt -match "S95 ([\d.]+) m2") { $s95 = $Matches[1] } else { $s95 = "0" }
    # The log has two S95 mentions; take the last one from the summary line.
    $s95lines = [regex]::Matches($txt, "S95 ([\d.]+) m2")
    if ($s95lines.Count -gt 0) { $s95 = $s95lines[$s95lines.Count - 1].Groups[1].Value }
    $rays = 3950.0 * [double]$Spp / 1e6
    $mray = if ([double]$flux -gt 0) { [math]::Round($rays / ([double]$flux * 1e-3), 0) } else { 0 }
    $row = "$Name,$Spp,$(if ($ExtraArgs -contains '--no-cull') {'0'} else {'1'}),$(if ($ExtraArgs -contains '--atomics') {'1'} else {'0'})," +
           "$([math]::Round($rays,3)),$deform,$flux,$scene,$([math]::Round($bloom,4)),$post,$total,$wall,$fps,$mray,$s95"
    $rows.Add($row)
    Write-Host ("  {0,-16} spp {1,4}  GPU {2,7} ms (flux {3,7})  wall {4,7} ms  {5,6} FPS  {6,8} Mray/s" -f `
                $Name, $Spp, $total, $flux, $wall, $fps, $mray)
}

Write-Host "spp ladder ($Present present, $Frames frames each)"
$ladder = if ($Quick) { @(16, 64, 256, 1024) } else { @(16, 32, 64, 128, 256, 512, 1024) }
foreach ($s in $ladder) { Run-Case -Name "spp$s" -Spp "$s" -ExtraArgs @() }

Write-Host "A/B switches (1024 spp)"
Run-Case -Name "ab_reference" -Spp "1024" -ExtraArgs @("--reference-chain")
Run-Case -Name "ab_atomics" -Spp "1024" -ExtraArgs @("--atomics")
Run-Case -Name "ab_nocull" -Spp "1024" -ExtraArgs @("--no-cull")
Run-Case -Name "ab_atomics64" -Spp "64" -ExtraArgs @("--atomics")

$rows | Set-Content -Path $Out -Encoding UTF8
Write-Host "wrote $Out ($($rows.Count - 1) configurations)"
Pop-Location
