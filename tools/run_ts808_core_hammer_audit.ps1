param(
    [int] $HammersteinTaps = 64,
    [string] $HammersteinOrders = "1,2,3,5",
    [int] $ChunkSamples = 16384,
    [int] $FitNfft = 1024,
    [int] $FitBands = 48
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location -LiteralPath $root

$outRoot = 'analysis_out\ts808_core_hammer_audit'
$renderDir = Join-Path $outRoot 'renders'
$stateOut = Join-Path $outRoot 'core_hammer_state.json'
$cascadeOut = Join-Path $outRoot 'core_hammer_cascade.csv'
New-Item -ItemType Directory -Path $renderDir -Force | Out-Null
Copy-Item -LiteralPath 'SAT-TR\tools\overdrive_id_renders\ts808_drive_drv100_in_p0__target.wav' -Destination (Join-Path $renderDir 'ts808_drive_drv100_in_p0__target.wav') -Force

python SAT-TR\tools\prepare_overdrive_core_hammer_state.py `
    --source SAT-TR\tools\overdrive_voicing_state.json `
    --state-out $stateOut `
    --cascade-out $cascadeOut

python SAT-TR\tools\run_overdrive_analysis_suite.py `
    --render-plan SAT-TR\tools\overdrive_id_renders\render_plan_ts808.json `
    --stim-dir SAT-TR\tools\overdrive_id_stimuli `
    --render-dir $renderDir `
    --out-root $outRoot `
    --sat-renderer-exe SAT-TR\tools\sat_overdrive_renderer\SatOverdriveRender.exe `
    --force-render-sat `
    --ts-cascade-csv $cascadeOut `
    --voicing-state $stateOut `
    --sat-render-mode voiced `
    --fit-nfft $FitNfft `
    --fit-bands $FitBands `
    --fit-layout ndsp-foundation-eq `
    --fit-basis-q 0.85 `
    --fit-max-gain-db 12.0 `
    --fit-grid-points 256 `
    --foundation-prefilter-limit 48 `
    --foundation-exact-limit 8 `
    --fit-only `
    --hammerstein-orders $HammersteinOrders `
    --hammerstein-taps $HammersteinTaps `
    --hammerstein-chunk-samples $ChunkSamples `
    --no-fit-plot

$hammerCsv = Join-Path $outRoot 'overdrive_cases\ts808_drive_drv100_in_p0\overdrive_id_fit_voiced\hammerstein_branch_summary.csv'
if (Test-Path -LiteralPath $hammerCsv -PathType Leaf) {
    python SAT-TR\tools\summarize_overdrive_hammerstein.py --csv $hammerCsv --out (Join-Path $outRoot 'hammerstein_summary')
}
else {
    Write-Warning "Hammerstein branch CSV not found: $hammerCsv"
}
