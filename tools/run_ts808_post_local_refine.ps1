param(
    [int] $Iterations = 1,
    [int] $MaxCandidates = 48,
    [double] $GainStepDb = 0.125,
    [int] $FoundationExactLimit = 8,
    [int] $Jobs = 2
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location -LiteralPath $root

python SAT-TR\tools\iterate_overdrive_ts808.py `
    --iterations $Iterations `
    --strict-missing `
    --pre-cascade-fit off `
    --post-local-fit verified `
    --post-local-max-candidates $MaxCandidates `
    --post-local-gain-step-db $GainStepDb `
    --candidate-residuals none `
    --core-mode off `
    --control-fit off `
    --jobs $Jobs `
    --foundation-exact-limit $FoundationExactLimit
$iterationExitCode = $LASTEXITCODE

if ($iterationExitCode -eq 0) {
    $exportScript = 'E:\Workspace\Production\JUCE_projects\SAT-TR\tools\export_overdrive_voicing_header.ps1'
    if (Test-Path -LiteralPath $exportScript -PathType Leaf) {
        powershell -ExecutionPolicy Bypass -File $exportScript
    }
    else {
        Write-Warning "Overdrive voicing export script not found, export skipped: $exportScript"
    }
}
else {
    Write-Warning "TS808 post-local refine failed with code $iterationExitCode; overdrive voicing export skipped."
}

exit $iterationExitCode
