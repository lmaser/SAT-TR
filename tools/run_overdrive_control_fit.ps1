param(
    [ValidateSet('ts808', 'klon')]
    [string] $Pedal = 'ts808',
    [string] $DriveGrid = '0.75,0.90,1.00,1.10,1.25,1.40',
    [string] $InputDbGrid = '-6,-3,-2,-1,0,1,2,3,6',
    [string] $OutputDbGrid = '0',
    [int] $MaxSegmentsPerKind = 3,
    [int] $Nfft = 4096,
    [int] $BlockSize = 1024
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location -LiteralPath $root

$typeValue = if ($Pedal -eq 'klon') { '1.0' } else { '0.0' }
$caseId = if ($Pedal -eq 'klon') { 'klon_drive_drv100_in_p0' } else { 'ts808_drive_drv100_in_p0' }
$renderPlan = "SAT-TR/tools/overdrive_id_renders/render_plan_$Pedal.json"
$outRoot = "analysis_out/${Pedal}_control_fit_current"

python SAT-TR\tools\fit_overdrive_ts808_controls.py `
    --render-plan $renderPlan `
    --case-id $caseId `
    --stim-dir SAT-TR/tools/overdrive_id_stimuli `
    --render-dir SAT-TR/tools/overdrive_id_renders `
    --sat-renderer-exe SAT-TR/tools/sat_overdrive_renderer/SatOverdriveRender.exe `
    --out $outRoot `
    "--drive-grid=$DriveGrid" `
    "--input-db-grid=$InputDbGrid" `
    "--output-db-grid=$OutputDbGrid" `
    "--knee-grid=0" `
    --type $typeValue `
    --max-segments-per-kind $MaxSegmentsPerKind `
    --nfft $Nfft `
    --block-size $BlockSize
$exitCode = $LASTEXITCODE

if ($exitCode -eq 0) {
    Write-Host "Control-fit summary: $outRoot/control_fit_summary.json"
    Write-Host "Control-fit CSV:     $outRoot/control_fit_results.csv"
}
exit $exitCode
