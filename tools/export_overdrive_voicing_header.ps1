param(
    [string] $Source = "C:\Users\Dev\Documents\SAT-TR-CascadeVoicing\SAT-TR\Source\OverdriveVoicingData.h",
    [string] $Destination = "E:\Workspace\Production\JUCE_projects\SAT-TR\Source\OverdriveVoicingData.h",
    [string] $StateSource = "C:\Users\Dev\Documents\SAT-TR-CascadeVoicing\SAT-TR\tools\overdrive_voicing_state.json",
    [string] $StateDestination = "E:\Workspace\Production\JUCE_projects\SAT-TR\tools\overdrive_voicing_state.json",
    [string] $PlotScript = "E:\Workspace\Production\JUCE_projects\SAT-TR\tools\plot_overdrive_voicing.py",
    [string] $PlotOutput = "E:\Workspace\Production\JUCE_projects\SAT-TR\tools\analysis_plots_current",
    [string] $Ts808ResidualCsv = "C:\Users\Dev\Documents\SAT-TR-CascadeVoicing\analysis_out\ts808_core_residual\current_state_report\overdrive_cases\ts808_drive_drv100_in_p0\overdrive_id_fit_voiced\residual_curves.csv",
    [string] $KlonResidualCsv = "C:\Users\Dev\Documents\SAT-TR-CascadeVoicing\analysis_out\klon_current_state_after_latest\overdrive_cases\klon_drive_drv100_in_p0\overdrive_id_fit_voiced\residual_curves.csv",
    [int] $MinBytes = 1024,
    [switch] $SkipRendererRebuild
)

$ErrorActionPreference = 'Stop'

function Warn-And-Exit([string] $Message) {
    Write-Warning $Message
    exit 0
}

try {
    if (-not (Test-Path -LiteralPath $StateSource -PathType Leaf)) {
        Warn-And-Exit "Overdrive voicing state not found, export skipped: $StateSource"
    }

    $stateToolsDir = Split-Path -Parent $StateSource
    $generator = Join-Path $stateToolsDir 'write_overdrive_voicing_header.py'
    if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
        Warn-And-Exit "Overdrive voicing generator not found, export skipped: $generator"
    }

    & python $generator
    if ($LASTEXITCODE -ne 0) {
        Warn-And-Exit "Overdrive voicing header regeneration failed with code $LASTEXITCODE; export skipped."
    }

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        Warn-And-Exit "Overdrive voicing header not found, export skipped: $Source"
    }

    $sourceItem = Get-Item -LiteralPath $Source
    if ($sourceItem.Length -lt $MinBytes) {
        Warn-And-Exit "Overdrive voicing header is unexpectedly small ($($sourceItem.Length) bytes), export skipped: $Source"
    }

    $destinationDir = Split-Path -Parent $Destination
    if (-not (Test-Path -LiteralPath $destinationDir -PathType Container)) {
        Warn-And-Exit "Destination directory not found, export skipped: $destinationDir"
    }

    $stateDestinationDir = Split-Path -Parent $StateDestination
    if (-not (Test-Path -LiteralPath $stateDestinationDir -PathType Container)) {
        Warn-And-Exit "State destination directory not found, export skipped: $stateDestinationDir"
    }

    $temp = "$Destination.tmp"
    Copy-Item -LiteralPath $Source -Destination $temp -Force
    Move-Item -LiteralPath $temp -Destination $Destination -Force

    $stateTemp = "$StateDestination.tmp"
    Copy-Item -LiteralPath $StateSource -Destination $stateTemp -Force
    Move-Item -LiteralPath $stateTemp -Destination $StateDestination -Force

    $srcHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $dstHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($srcHash -ne $dstHash) {
        Warn-And-Exit "Export hash mismatch, destination may not match source. Source=$srcHash Destination=$dstHash"
    }

    $stateSrcHash = (Get-FileHash -LiteralPath $StateSource -Algorithm SHA256).Hash
    $stateDstHash = (Get-FileHash -LiteralPath $StateDestination -Algorithm SHA256).Hash
    if ($stateSrcHash -ne $stateDstHash) {
        Warn-And-Exit "State export hash mismatch, destination may not match source. Source=$stateSrcHash Destination=$stateDstHash"
    }

    if (-not $SkipRendererRebuild) {
        $rendererBuildScript = Join-Path $stateToolsDir 'sat_overdrive_renderer\build_sat_overdrive_renderer.ps1'
        if (Test-Path -LiteralPath $rendererBuildScript -PathType Leaf) {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $rendererBuildScript
            if ($LASTEXITCODE -ne 0) {
                Warn-And-Exit "Overdrive renderer rebuild failed with code $LASTEXITCODE; export kept but renderer may be stale."
            }
        }
        else {
            Write-Warning "Overdrive renderer build script not found, renderer rebuild skipped: $rendererBuildScript"
        }
    }

    Write-Host "Overdrive voicing exported OK:"
    Write-Host "  Header source:      $Source"
    Write-Host "  Header destination: $Destination"
    Write-Host "  Header SHA256:      $dstHash"
    Write-Host "  State source:       $StateSource"
    Write-Host "  State destination:  $StateDestination"
    Write-Host "  State SHA256:       $stateDstHash"

    if (Test-Path -LiteralPath $PlotScript -PathType Leaf) {
        $plotArgs = @(
            $PlotScript,
            '--state', $StateDestination,
            '--out', $PlotOutput,
            '--sample-rate', '48000',
            '--points', '4096'
        )

        if (Test-Path -LiteralPath $Ts808ResidualCsv -PathType Leaf) {
            $plotArgs += @('--ts808-residual-csv', $Ts808ResidualCsv)
        }
        if (Test-Path -LiteralPath $KlonResidualCsv -PathType Leaf) {
            $plotArgs += @('--klon-residual-csv', $KlonResidualCsv)
        }

        & python @plotArgs
        if ($LASTEXITCODE -eq 0) {
            Write-Host "  Plot output:        $PlotOutput"
        }
        else {
            Write-Warning "Overdrive plot generation failed with code $LASTEXITCODE; export kept."
        }
    }
    else {
        Write-Warning "Overdrive plot script not found, plots skipped: $PlotScript"
    }

    exit 0
}
catch {
    try {
        $temp = "$Destination.tmp"
        if (Test-Path -LiteralPath $temp -PathType Leaf) {
            Remove-Item -LiteralPath $temp -Force
        }
        $stateTemp = "$StateDestination.tmp"
        if (Test-Path -LiteralPath $stateTemp -PathType Leaf) {
            Remove-Item -LiteralPath $stateTemp -Force
        }
    }
    catch {
    }

    Warn-And-Exit "Overdrive voicing export failed and was skipped: $($_.Exception.Message)"
}
