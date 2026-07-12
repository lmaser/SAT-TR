param(
    [int] $Iterations = 3,
    [int] $PreCascadeMaxCandidates = 96,
    [int] $PreCascadeScreenLimit = 16,
    [int] $ResidualBaseMaxCandidates = 4,
    [int] $Jobs = 1,
    [string] $Mode = 'core-current-no-cascade',
    [switch] $NoRestore,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $ExtraArgs
)

$ErrorActionPreference = 'Stop'
$toolsRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$satRoot = Split-Path -Parent $toolsRoot
$suiteRoot = Split-Path -Parent $satRoot
Set-Location -LiteralPath $suiteRoot

function Invoke-Checked {
    param(
        [string] $File,
        [string[]] $Arguments
    )
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $File $($Arguments -join ' ')"
    }
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$auditRoot = Join-Path $suiteRoot 'analysis_out\ts808_replay_audit'
$snapshotRoot = Join-Path $auditRoot ("_snapshots\" + $stamp)
New-Item -ItemType Directory -Force -Path $snapshotRoot | Out-Null

$statePath = Join-Path $suiteRoot 'SAT-TR\tools\overdrive_voicing_state.json'
$headerPath = Join-Path $suiteRoot 'SAT-TR\Source\OverdriveVoicingData.h'
$stateBackup = Join-Path $snapshotRoot 'overdrive_voicing_state.original.json'
$headerBackup = Join-Path $snapshotRoot 'OverdriveVoicingData.original.h'
$replayState = Join-Path $snapshotRoot 'overdrive_voicing_state.replay.json'
$metaJson = Join-Path $auditRoot 'meta_report.json'

Copy-Item -LiteralPath $statePath -Destination $stateBackup -Force
if (Test-Path -LiteralPath $headerPath -PathType Leaf) {
    Copy-Item -LiteralPath $headerPath -Destination $headerBackup -Force
}

try {
    Invoke-Checked 'python' @('SAT-TR\tools\prepare_overdrive_replay_state.py', '--source', $statePath, '--out', $replayState, '--mode', $Mode)
    Copy-Item -LiteralPath $replayState -Destination $statePath -Force
    Invoke-Checked 'python' @('SAT-TR\tools\write_overdrive_voicing_header.py')
    Invoke-Checked 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', 'SAT-TR\tools\sat_overdrive_renderer\build_sat_overdrive_renderer.ps1')

    $iterateArgs = @(
        '--iterations', [string]$Iterations,
        '--strict-missing',
        '--out-root', 'analysis_out/ts808_replay_audit',
        '--baseline', 'analysis_out/ts808_replay_audit/overdrive_fit_baselines.json',
        '--pre-cascade-max-candidates', [string]$PreCascadeMaxCandidates,
        '--pre-cascade-screen-limit', [string]$PreCascadeScreenLimit,
        '--residual-base-max-candidates', [string]$ResidualBaseMaxCandidates,
        '--core-mode', 'off',
        '--control-fit', 'off',
        '--jobs', [string]$Jobs,
        '--foundation-exact-limit', '12',
        '--pre-cascade-gain-step-db', '0.25',
        '--no-build-renderer'
    )
    if ($ExtraArgs) {
        $iterateArgs += $ExtraArgs
    }

    python SAT-TR\tools\iterate_overdrive_ts808.py @iterateArgs
    $iterationExitCode = $LASTEXITCODE
    python SAT-TR\tools\meta_overdrive_ts808.py --analysis-root analysis_out/ts808_replay_audit --state SAT-TR/tools/overdrive_voicing_state.json --json-out $metaJson
    exit $iterationExitCode
}
finally {
    if (-not $NoRestore) {
        Copy-Item -LiteralPath $stateBackup -Destination $statePath -Force
        if (Test-Path -LiteralPath $headerBackup -PathType Leaf) {
            Copy-Item -LiteralPath $headerBackup -Destination $headerPath -Force
        }
        Write-Host "Replay audit restored original state/header. Snapshot: $snapshotRoot"
    }
    else {
        Write-Warning "NoRestore was set; replay state remains active in Documents copy. Snapshot: $snapshotRoot"
    }
}
