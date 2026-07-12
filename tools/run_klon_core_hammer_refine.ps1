param(
    [int] $Iterations = 1,
    [int] $MaxCandidates = 24,
    [int] $CorePairCandidates = 0,
    [string] $Profile = 'fast',
    [double] $StaticWorsening = 0.020,
    [double] $HammerRelativeImprovement = 0.002,
    [double] $HammerAbsoluteImprovement = 0.025,
    [int] $FoundationExactLimit = 8,
    [int] $CoupledPreMaxCandidates = 0,
    [int] $CoupledPreStructuralMaxCandidates = 0,
    [int] $CoupledPreStructuralSourceRows = 6,
    [string] $CoupledPreStructuralLayers = 'pre_a,pre_b',
    [string] $CoupledPreStructuralGainScales = '0.35,0.7,-0.35,-0.7',
    [string] $CoupledPreDeltas = '-0.25,-0.125,0.125,0.25',
    [int] $CoupledPostMaxCandidates = 0,
    [string] $CoupledPostDeltas = '-0.25,-0.125,0.125,0.25',
    [int] $CoupledRefitPasses = 1,
    [switch] $CoupledFitPostReplace,
    [string] $OnlyParams = '',
    [string] $OutRoot = 'C:\tmp\klon_core_hammer_close',
    [switch] $Quiet
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
Set-Location -LiteralPath $root

$extraArgs = @()
if ($CoupledFitPostReplace) { $extraArgs += '--coupled-fit-post-replace' }
if ($Quiet) { $extraArgs += '--quiet' }

python SAT-TR\tools\refine_klon_core_hammer.py `
    --iterations $Iterations `
    --max-candidates $MaxCandidates `
    --core-pair-candidates $CorePairCandidates `
    --profile $Profile `
    --static-worsening $StaticWorsening `
    --hammer-relative-improvement $HammerRelativeImprovement `
    --hammer-absolute-improvement $HammerAbsoluteImprovement `
    --foundation-exact-limit $FoundationExactLimit `
    --coupled-pre-max-candidates $CoupledPreMaxCandidates `
    --coupled-pre-structural-max-candidates $CoupledPreStructuralMaxCandidates `
    --coupled-pre-structural-source-rows $CoupledPreStructuralSourceRows `
    --coupled-pre-structural-layers=$CoupledPreStructuralLayers `
    --coupled-pre-structural-gain-scales=$CoupledPreStructuralGainScales `
    --coupled-pre-deltas=$CoupledPreDeltas `
    --coupled-refit-passes $CoupledRefitPasses `
    --coupled-post-max-candidates $CoupledPostMaxCandidates `
    --coupled-post-deltas=$CoupledPostDeltas `
    --only-params=$OnlyParams `
    --out-root $OutRoot `
    @extraArgs `
    --apply
$refineExitCode = $LASTEXITCODE

if ($refineExitCode -eq 0) {
    $exportScript = 'E:\Workspace\Production\JUCE_projects\SAT-TR\tools\export_overdrive_voicing_header.ps1'
    if (Test-Path -LiteralPath $exportScript -PathType Leaf) {
        powershell -ExecutionPolicy Bypass -File $exportScript
    }
    else {
        Write-Warning "Overdrive voicing export script not found, export skipped: $exportScript"
    }
}
else {
    Write-Warning "Klon core-Hammer refine failed with code $refineExitCode; overdrive voicing export skipped."
}

exit $refineExitCode
