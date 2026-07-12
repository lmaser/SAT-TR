param(
    [string] $OutCsv = "",
    [int] $SampleRate = 48000,
    [double] $InputPeak = 0.5,
    [double] $ToneHz = 997.0,
    [ValidateSet("sine", "white", "pink", "brown")]
    [string] $SignalType = "sine",
    [uint32] $Seed = 305419896,
    [switch] $RawMode,
    [int] $WarmupSamples = 48000,
    [int] $MeasureSamples = 48000
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$sourceDir = Join-Path $repo "Source"

if ([string]::IsNullOrWhiteSpace($OutCsv)) {
    $OutCsv = Join-Path (Join-Path $repo ".level-measure") "sat_level_measurements.csv"
}

$outDir = Split-Path -Parent $OutCsv
if ([string]::IsNullOrWhiteSpace($outDir)) {
    $outDir = Join-Path $repo ".level-measure"
}

$workName = [System.IO.Path]::GetFileNameWithoutExtension($OutCsv)
if ([string]::IsNullOrWhiteSpace($workName)) {
    $workName = "sat_level_measurements"
}

$workDir = Join-Path (Join-Path $repo ".level-measure") $workName
$stubDir = Join-Path $workDir "stub"
New-Item -ItemType Directory -Force -Path $outDir, $workDir, $stubDir | Out-Null

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
$signalTypeForCpp = $SignalType.ToLowerInvariant()
$seedForCpp = ([uint32] $Seed).ToString([System.Globalization.CultureInfo]::InvariantCulture)
$rawModeForCpp = if ($RawMode.IsPresent) { "true" } else { "false" }

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

static unsigned int rngNext (unsigned int& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static float randBipolar (unsigned int& state)
{
    const unsigned int v = rngNext (state);
    return ((float) (v & 0x00FFFFFFu) / 8388607.5f) - 1.0f;
}

static void generateSignal (std::vector<float>& left,
                            std::vector<float>& right,
                            const char* signalType,
                            int sampleRate,
                            double toneHz,
                            double inputPeak,
                            unsigned int seed)
{
    const int totalSamples = (int) left.size();
    double maxAbs = 0.0;

    if (std::strcmp (signalType, "white") == 0)
    {
        unsigned int s = seed != 0u ? seed : 0x12345678u;
        for (int i = 0; i < totalSamples; ++i)
        {
            const float x = randBipolar (s);
            left[(size_t) i] = x;
            right[(size_t) i] = x;
            maxAbs = std::max (maxAbs, std::abs ((double) x));
        }
    }
    else if (std::strcmp (signalType, "pink") == 0)
    {
        // Paul Kellet style pinking filter: deterministic and cheap for trim calibration.
        unsigned int s = seed != 0u ? seed : 0x12345678u;
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            const double white = (double) randBipolar (s);
            b0 = 0.99886 * b0 + white * 0.0555179;
            b1 = 0.99332 * b1 + white * 0.0750759;
            b2 = 0.96900 * b2 + white * 0.1538520;
            b3 = 0.86650 * b3 + white * 0.3104856;
            b4 = 0.55000 * b4 + white * 0.5329522;
            b5 = -0.7616 * b5 - white * 0.0168980;
            const double y = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
            b6 = white * 0.115926;
            const float x = (float) (y * 0.11);
            left[(size_t) i] = x;
            right[(size_t) i] = x;
            maxAbs = std::max (maxAbs, std::abs ((double) x));
        }
    }
    else if (std::strcmp (signalType, "brown") == 0)
    {
        unsigned int s = seed != 0u ? seed : 0x12345678u;
        double acc = 0.0;
        for (int i = 0; i < totalSamples; ++i)
        {
            // Leaky Brownian noise: low-frequency weighted, bounded, deterministic.
            acc = acc * 0.9975 + (double) randBipolar (s) * 0.055;
            acc = std::max (-1.0, std::min (1.0, acc));
            const float x = (float) acc;
            left[(size_t) i] = x;
            right[(size_t) i] = x;
            maxAbs = std::max (maxAbs, std::abs (acc));
        }
    }
    else
    {
        for (int i = 0; i < totalSamples; ++i)
        {
            const float x = (float) std::sin (2.0 * SatEngine::kPi * toneHz * (double) i / (double) sampleRate);
            left[(size_t) i] = x;
            right[(size_t) i] = x;
            maxAbs = std::max (maxAbs, std::abs ((double) x));
        }
    }

    const float scale = (float) (inputPeak / std::max (1.0e-12, maxAbs));
    for (int i = 0; i < totalSamples; ++i)
    {
        left[(size_t) i] *= scale;
        right[(size_t) i] *= scale;
    }
}

static Stats measure (SatEngine::Model model,
                      float drive, float character, float type, float bias, float react,
                      int seriesCount,
                      int sampleRate,
                      double toneHz,
                      double inputPeak,
                      const char* signalType,
                      unsigned int seed,
                      int warmupSamples,
                      int measureSamples)
{
    auto state = std::make_unique<SatEngine::State>();
    const int totalSamples = warmupSamples + measureSamples;
    std::vector<float> left ((size_t) totalSamples);
    std::vector<float> right ((size_t) totalSamples);
    generateSignal (left, right, signalType, sampleRate, toneHz, inputPeak, seed);
    const std::vector<float> input = left;

    SatEngine::processBlock (*state,
                             left.data(), right.data(), totalSamples,
                             model,
                             drive, character, type, bias, react,
                             0.0f, 0.0f,
                             (float) sampleRate,
                             seriesCount,
                             true,
                             $rawModeForCpp,
                             nullptr);

    Stats s;
    for (int i = warmupSamples; i < totalSamples; ++i)
    {
        const double in = (double) input[(size_t) i];
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

    std::fprintf (f, "signal,model,drive,char,type,bias,react,series,inputPeak,rmsIn,rmsOut,deltaDb,peakIn,peakOut,peakDeltaDb\n");

    struct ModelDef { SatEngine::Model model; const char* name; };
    const ModelDef models[] = {
        { SatEngine::Model::Tape, "Tape" },
        { SatEngine::Model::Tube, "Tube" },
        { SatEngine::Model::Transistor, "Transistor" },
        { SatEngine::Model::Diode, "Diode" },
        { SatEngine::Model::OverdriveA, "OverdriveA" }
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
                                             "$signalTypeForCpp", $seedForCpp,
                                             $WarmupSamples, $MeasureSamples);
                        const double deltaDb = db (s.rmsOut / s.rmsIn);
                        const double peakDeltaDb = db (s.peakOut / s.peakIn);
                        std::fprintf (f, "%s,%s,%.2f,%.2f,%.2f,0.00,0.00,%d,%.6f,%.9f,%.9f,%.4f,%.9f,%.9f,%.4f\n",
                                      "$signalTypeForCpp", m.name, drive, character, type, series, $inputPeakForCpp,
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
