param(
    [int] $Iterations = 1,
    [string] $CandidateResiduals = 'replace:0.25,replace:0.5,replace:0.75',
    [int] $FoundationExactLimit = 8
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location -LiteralPath $root

python SAT-TR\tools\iterate_overdrive_ts808.py `
    --iterations $Iterations `
    --strict-missing `
    --pre-cascade-fit off `
    --core-mode off `
    --control-fit off `
    --candidate-residuals $CandidateResiduals `
    --residual-base-max-candidates 0 `
    --jobs 1 `
    --foundation-exact-limit $FoundationExactLimit `
    --pre-cascade-gain-step-db 0.25
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
    Write-Warning "TS808 post-only refine failed with code $iterationExitCode; overdrive voicing export skipped."
}

exit $iterationExitCode
