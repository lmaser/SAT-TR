param(
    [string] $OutCsv = "",
    [int] $SampleRate = 48000,
    [double] $InputPeak = 0.5,
    [double] $ToneHz = 997.0,
    [int] $WarmupSamples = 48000,
    [int] $MeasureSamples = 48000
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sourceDir = Join-Path $repo "Source"
$workDir = Join-Path $repo ".level-measure"
$stubDir = Join-Path $workDir "stub"
New-Item -ItemType Directory -Force -Path $workDir, $stubDir | Out-Null

if ([string]::IsNullOrWhiteSpace($OutCsv)) {
    $OutCsv = Join-Path $workDir "sat_level_measurements.csv"
}

$juceStub = @'
#pragma once
#include <algorithm>
#include <cstdint>
namespace juce
{
    using int64 = long long;

    template <typename T>
    inline T jlimit (T low, T high, T value) noexcept
    {
        return std::min (high, std::max (low, value));
    }

    template <typename T>
    inline T jmax (T a, T b) noexcept
    {
        return std::max (a, b);
    }

    template <typename T, typename U>
    inline auto jmap (T proportion, U start, U end) noexcept -> decltype (start + (end - start) * proportion)
    {
        return start + (end - start) * proportion;
    }

    template <typename... Args>
    inline void ignoreUnused (Args&&...) noexcept {}
}
'@
Set-Content -Path (Join-Path $stubDir "JuceHeader.h") -Value $juceStub -Encoding ASCII

$harnessPath = Join-Path $workDir "sat_level_harness.cpp"
$objPath = Join-Path $workDir "sat_level_harness.obj"
$exePath = Join-Path $workDir "sat_level_harness.exe"
$csvForCpp = $OutCsv.Replace('\', '\\')
$sourceForCpp = $sourceDir.Replace('\', '\\')
$inputPeakForCpp = ([double] $InputPeak).ToString("R", [System.Globalization.CultureInfo]::InvariantCulture)
if ($inputPeakForCpp -notmatch '[\.eE]') {
    $inputPeakForCpp += ".0"
}

$harness = @"
#define SAT_DSP_DIAG 0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include "SaturationEngine.h"

static double db (double x)
{
    return 20.0 * std::log10 (std::max (1.0e-12, std::abs (x)));
}

struct Stats
{
    double rmsIn = 0.0;
    double rmsOut = 0.0;
    double peakIn = 0.0;
    double peakOut = 0.0;
};

static Stats measure (SatEngine::Model model,
                      float drive, float character, float type, float bias, float react,
                      int seriesCount,
                      int sampleRate,
                      double toneHz,
                      double inputPeak,
                      int warmupSamples,
                      int measureSamples)
{
    auto state = std::make_unique<SatEngine::State>();
    const int totalSamples = warmupSamples + measureSamples;
    std::vector<float> left ((size_t) totalSamples);
    std::vector<float> right ((size_t) totalSamples);

    for (int i = 0; i < totalSamples; ++i)
    {
        const float x = (float) (inputPeak * std::sin (2.0 * SatEngine::kPi * toneHz * (double) i / (double) sampleRate));
        left[(size_t) i] = x;
        right[(size_t) i] = x;
    }

    SatEngine::processBlock (*state,
                             left.data(), right.data(), totalSamples,
                             model,
                             drive, character, type, bias, react,
                             0.0f, 0.0f,
                             (float) sampleRate,
                             seriesCount,
                             true,
                             false,
                             nullptr);

    Stats s;
    for (int i = warmupSamples; i < totalSamples; ++i)
    {
        const double in = inputPeak * std::sin (2.0 * SatEngine::kPi * toneHz * (double) i / (double) sampleRate);
        const double out = (double) left[(size_t) i];
        s.rmsIn += in * in;
        s.rmsOut += out * out;
        s.peakIn = std::max (s.peakIn, std::abs (in));
        s.peakOut = std::max (s.peakOut, std::abs (out));
    }

    s.rmsIn = std::sqrt (s.rmsIn / (double) measureSamples);
    s.rmsOut = std::sqrt (s.rmsOut / (double) measureSamples);
    return s;
}

int main()
{
    FILE* f = nullptr;
    if (fopen_s (&f, "$csvForCpp", "wb") != 0 || f == nullptr)
        return 2;

    std::fprintf (f, "model,drive,char,type,bias,react,series,inputPeak,rmsIn,rmsOut,deltaDb,peakIn,peakOut,peakDeltaDb\n");

    struct ModelDef { SatEngine::Model model; const char* name; };
    const ModelDef models[] = {
        { SatEngine::Model::Tape, "Tape" },
        { SatEngine::Model::Tube, "Tube" },
        { SatEngine::Model::Transistor, "Transistor" },
        { SatEngine::Model::Diode, "Diode" },
        { SatEngine::Model::Clipper, "Clipper" }
    };

    const float drives[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    const float chars[] = { 0.0f, 0.5f, 1.0f };
    const float types[] = { 0.0f, 0.5f, 1.0f };
    const int seriesValues[] = { 1, 2, 3, 4 };

    for (const auto& m : models)
    {
        for (float drive : drives)
        {
            for (float character : chars)
            {
                for (float type : types)
                {
                    for (int series : seriesValues)
                    {
                        const Stats s = measure (m.model, drive, character, type, 0.0f, 0.0f, series,
                                             $SampleRate, $ToneHz, $inputPeakForCpp,
                                             $WarmupSamples, $MeasureSamples);
                        const double deltaDb = db (s.rmsOut / s.rmsIn);
                        const double peakDeltaDb = db (s.peakOut / s.peakIn);
                        std::fprintf (f, "%s,%.2f,%.2f,%.2f,0.00,0.00,%d,%.6f,%.9f,%.9f,%.4f,%.9f,%.9f,%.4f\n",
                                      m.name, drive, character, type, series, $inputPeakForCpp,
                                      s.rmsIn, s.rmsOut, deltaDb,
                                      s.peakIn, s.peakOut, peakDeltaDb);
                    }
                }
            }
        }
    }

    std::fclose (f);
    return 0;
}
"@
Set-Content -Path $harnessPath -Value $harness -Encoding ASCII

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Run from a Visual Studio Developer PowerShell or install VS Build Tools."
}

$vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($vsInstall)) {
    throw "MSVC toolchain not found by vswhere."
}

$vcvars = Join-Path $vsInstall "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at $vcvars"
}

$cmd = "`"$vcvars`" >nul && cl /nologo /std:c++17 /EHsc /O2 /DNOMINMAX /I`"$stubDir`" /I`"$sourceDir`" `"$harnessPath`" /Fo:`"$objPath`" /Fe:`"$exePath`""
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Harness compilation failed."
}

& $exePath
if ($LASTEXITCODE -ne 0) {
    throw "Harness execution failed."
}

Write-Host "Wrote $OutCsv"
