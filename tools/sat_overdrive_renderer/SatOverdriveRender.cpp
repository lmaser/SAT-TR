#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

#define SAT_DSP_DIAG 0
#define SAT_TS808_RUNTIME_TUNING 1
#include "JuceHeader.h"
#include "../../Source/SaturationEngine.h"

namespace
{
struct AudioFile
{
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples; // interleaved
};

float dbToGain(float db) noexcept { return std::pow(10.0f, db / 20.0f); }

uint16_t readU16(std::ifstream& in)
{
    uint8_t b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return uint16_t(b[0] | (uint16_t(b[1]) << 8));
}

uint32_t readU32(std::ifstream& in)
{
    uint8_t b[4]{};
    in.read(reinterpret_cast<char*>(b), 4);
    return uint32_t(b[0] | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24));
}

void writeU16(std::ofstream& out, uint16_t v)
{
    uint8_t b[2] = { uint8_t(v & 0xff), uint8_t((v >> 8) & 0xff) };
    out.write(reinterpret_cast<const char*>(b), 2);
}

void writeU32(std::ofstream& out, uint32_t v)
{
    uint8_t b[4] = { uint8_t(v & 0xff), uint8_t((v >> 8) & 0xff), uint8_t((v >> 16) & 0xff), uint8_t((v >> 24) & 0xff) };
    out.write(reinterpret_cast<const char*>(b), 4);
}

int32_t signExtend24(uint32_t v)
{
    if (v & 0x800000u)
        v |= 0xff000000u;
    return static_cast<int32_t>(v);
}

AudioFile readWavUncached(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open input WAV: " + path);

    char id[4]{};
    in.read(id, 4);
    if (std::strncmp(id, "RIFF", 4) != 0) throw std::runtime_error("not RIFF WAV: " + path);
    (void) readU32(in);
    in.read(id, 4);
    if (std::strncmp(id, "WAVE", 4) != 0) throw std::runtime_error("not WAVE: " + path);

    uint16_t audioFormat = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    std::vector<uint8_t> data;

    while (in && !in.eof())
    {
        in.read(id, 4);
        if (in.gcount() != 4) break;
        const uint32_t size = readU32(in);
        const std::streampos next = in.tellg() + std::streamoff(size + (size & 1u));
        if (std::strncmp(id, "fmt ", 4) == 0)
        {
            audioFormat = readU16(in);
            channels = readU16(in);
            sampleRate = readU32(in);
            (void) readU32(in); // byte rate
            (void) readU16(in); // block align
            bitsPerSample = readU16(in);
        }
        else if (std::strncmp(id, "data", 4) == 0)
        {
            data.resize(size);
            in.read(reinterpret_cast<char*>(data.data()), size);
        }
        in.seekg(next);
    }

    if (channels == 0 || sampleRate == 0 || data.empty())
        throw std::runtime_error("incomplete WAV: " + path);

    AudioFile file;
    file.sampleRate = static_cast<int>(sampleRate);
    file.channels = static_cast<int>(channels);

    const int bytesPerSample = int(bitsPerSample / 8);
    const size_t frameCount = data.size() / size_t(bytesPerSample * channels);
    file.samples.resize(frameCount * channels);

    const uint8_t* p = data.data();
    for (size_t i = 0; i < frameCount * channels; ++i)
    {
        float v = 0.0f;
        if (audioFormat == 3 && bitsPerSample == 32)
        {
            float f{};
            std::memcpy(&f, p, 4);
            v = f;
        }
        else if (audioFormat == 1 && bitsPerSample == 16)
        {
            int16_t s = int16_t(uint16_t(p[0] | (uint16_t(p[1]) << 8)));
            v = float(s) / 32768.0f;
        }
        else if (audioFormat == 1 && bitsPerSample == 24)
        {
            uint32_t raw = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16);
            v = float(signExtend24(raw)) / 8388608.0f;
        }
        else if (audioFormat == 1 && bitsPerSample == 32)
        {
            int32_t s = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24));
            v = float(double(s) / 2147483648.0);
        }
        else
        {
            throw std::runtime_error("unsupported WAV format; use PCM16/24/32 or float32");
        }
        file.samples[i] = v;
        p += bytesPerSample;
    }
    return file;
}

const AudioFile& readWav(const std::string& path)
{
    static std::unordered_map<std::string, AudioFile> cache;
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second;

    auto inserted = cache.emplace(path, readWavUncached(path));
    return inserted.first->second;
}

void writeFloatWav(const std::string& path, const AudioFile& file)
{
    const std::string tmpPath = path + ".tmp";
    std::remove(tmpPath.c_str());

    const uint64_t dataBytes64 = uint64_t(file.samples.size()) * uint64_t(sizeof(float));
    if (dataBytes64 > uint64_t(UINT32_MAX))
        throw std::runtime_error("output WAV too large for RIFF32: " + path);

    const uint32_t dataBytes = uint32_t(dataBytes64);
    const uint32_t fmtBytes = 16;
    const uint16_t audioFormat = 3; // IEEE float
    const uint16_t bits = 32;
    const uint16_t blockAlign = uint16_t(file.channels * sizeof(float));
    const uint32_t byteRate = uint32_t(file.sampleRate * blockAlign);

    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot open temporary output WAV: " + tmpPath);
        out.write("RIFF", 4);
        writeU32(out, 4 + (8 + fmtBytes) + (8 + dataBytes));
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        writeU32(out, fmtBytes);
        writeU16(out, audioFormat);
        writeU16(out, uint16_t(file.channels));
        writeU32(out, uint32_t(file.sampleRate));
        writeU32(out, byteRate);
        writeU16(out, blockAlign);
        writeU16(out, bits);
        out.write("data", 4);
        writeU32(out, dataBytes);
        out.write(reinterpret_cast<const char*>(file.samples.data()), dataBytes);
        out.flush();
        if (!out.good())
        {
            out.close();
            std::remove(tmpPath.c_str());
            throw std::runtime_error("failed while writing output WAV: " + tmpPath);
        }
    }

    std::remove(path.c_str());
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0)
    {
        std::remove(tmpPath.c_str());
        throw std::runtime_error("cannot commit temporary output WAV: " + path);
    }
}

std::string argValue(int argc, char** argv, const std::string& name, const std::string& fallback = {})
{
    for (int i = 1; i + 1 < argc; ++i)
        if (argv[i] == name)
            return argv[i + 1];
    return fallback;
}

bool hasArg(int argc, char** argv, const std::string& name)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] == name)
            return true;
    return false;
}

float argFloat(int argc, char** argv, const std::string& name, float fallback)
{
    const auto v = argValue(argc, argv, name, "");
    return v.empty() ? fallback : std::stof(v);
}

int argInt(int argc, char** argv, const std::string& name, int fallback)
{
    const auto v = argValue(argc, argv, name, "");
    return v.empty() ? fallback : std::stoi(v);
}


std::string trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::vector<std::string> splitCsvLine(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (char c : line)
    {
        if (c == '"')
        {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted)
        {
            fields.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    fields.push_back(trim(current));
    return fields;
}

SatEngine::KlonEqKind parseKlonEqKind(const std::string& text)
{
    if (text == "Peak") return SatEngine::KlonEqKind::Peak;
    if (text == "LowShelf") return SatEngine::KlonEqKind::LowShelf;
    if (text == "HighShelf") return SatEngine::KlonEqKind::HighShelf;
    if (text == "LowPass") return SatEngine::KlonEqKind::LowPass;
    if (text == "HighPass") return SatEngine::KlonEqKind::HighPass;
    if (text == "TiltShelf") return SatEngine::KlonEqKind::TiltShelf;
    throw std::runtime_error("unsupported KlonEqKind in cascade CSV: " + text);
}

SatEngine::KlonEqAmount parseKlonEqAmount(const std::string& text)
{
    if (text.empty() || text == "Classic") return SatEngine::KlonEqAmount::Classic;
    if (text == "Fixed") return SatEngine::KlonEqAmount::Fixed;
    if (text == "Reference") return SatEngine::KlonEqAmount::Reference;
    if (text == "ClassicDrive") return SatEngine::KlonEqAmount::ClassicDrive;
    throw std::runtime_error("unsupported KlonEqAmount in cascade CSV: " + text);
}

std::vector<SatEngine::KlonEqBandSpec>* layerForName(const std::string& layer, const std::string& model)
{
    namespace O = SatEngine::OverdriveVoicing;
    const bool klon = model == "klon" || model == "overdrive-b" || model == "overdriveb";
    if (klon)
    {
        if (layer == "pre_a") return &O::mutableKlonPreAForAnalysis();
        if (layer == "pre_ndsp") return &O::mutableKlonPreNdspForAnalysis();
        if (layer == "pre_b") return &O::mutableKlonPreBForAnalysis();
        if (layer == "post_a") return &O::mutableKlonPostAForAnalysis();
        if (layer == "post_ndsp") return &O::mutableKlonPostNdspForAnalysis();
        if (layer == "post_b") return &O::mutableKlonPostBForAnalysis();
        return nullptr;
    }
    if (layer == "pre_a") return &O::mutableTs808PreAForAnalysis();
    if (layer == "pre_ndsp") return &O::mutableTs808PreNdspForAnalysis();
    if (layer == "pre_b") return &O::mutableTs808PreBForAnalysis();
    if (layer == "post_a") return &O::mutableTs808PostAForAnalysis();
    if (layer == "post_ndsp") return &O::mutableTs808PostNdspForAnalysis();
    if (layer == "post_b") return &O::mutableTs808PostBForAnalysis();
    return nullptr;
}

void applyTs808CascadeCsv(const std::string& path, const std::string& model = "ts808")
{
    if (path.empty())
        return;

    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open TS808 cascade CSV: " + path);

    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error("empty TS808 cascade CSV: " + path);

    const auto header = splitCsvLine(line);
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < header.size(); ++i)
        col[header[i]] = i;

    auto require = [&] (const std::string& name) -> size_t {
        auto it = col.find(name);
        if (it == col.end())
            throw std::runtime_error("TS808 cascade CSV missing column: " + name);
        return it->second;
    };

    const size_t cLayer = require("layer");
    const size_t cKind = require("kind");
    const size_t cFreq = require("freq_hz");
    const size_t cQ = require("q");
    const size_t cGain = require("gain_db");
    const size_t cStages = require("stages");
    const size_t cAmount = col.count("amount") ? col["amount"] : size_t(-1);

    std::vector<std::string> cleared;
    auto wasCleared = [&] (const std::string& layer) {
        return std::find(cleared.begin(), cleared.end(), layer) != cleared.end();
    };

    while (std::getline(in, line))
    {
        if (trim(line).empty())
            continue;
        const auto fields = splitCsvLine(line);
        const auto get = [&] (size_t idx) -> std::string {
            return idx < fields.size() ? fields[idx] : std::string();
        };

        const std::string layerName = get(cLayer);
        auto* layer = layerForName(layerName, model);
        if (layer == nullptr)
            throw std::runtime_error("unsupported TS808 cascade layer: " + layerName);
        if (!wasCleared(layerName))
        {
            layer->clear();
            cleared.push_back(layerName);
        }

        SatEngine::KlonEqBandSpec band{};
        band.kind = parseKlonEqKind(get(cKind));
        band.freqHz = std::stof(get(cFreq));
        band.q = std::stof(get(cQ));
        band.gainDb = std::stof(get(cGain));
        band.stages = std::stoi(get(cStages));
        band.amount = cAmount == size_t(-1) ? SatEngine::KlonEqAmount::Classic : parseKlonEqAmount(get(cAmount));
        layer->push_back(band);
    }
}

void applyTs808CoreOverride(int argc, char** argv)
{
    auto& ts = SatEngine::OverdriveVoicing::mutableTs808CoreForAnalysis();
    ts.driveScale = argFloat(argc, argv, "--ts-drive-scale", ts.driveScale);
    ts.driveHeadroom = argFloat(argc, argv, "--ts-drive-headroom", ts.driveHeadroom);
    ts.inputGainDb = argFloat(argc, argv, "--ts-input-gain-db", ts.inputGainDb);
    ts.loopDriveMax = argFloat(argc, argv, "--ts-loop-drive-max", ts.loopDriveMax);
    ts.loopCappedGainAtMaxDrive = argFloat(argc, argv, "--ts-loop-capped-gain", ts.loopCappedGainAtMaxDrive);
    ts.airGainAtMaxDrive = argFloat(argc, argv, "--ts-air-gain", ts.airGainAtMaxDrive);
    ts.solverKneeStart = argFloat(argc, argv, "--ts-solver-knee-start", ts.solverKneeStart);
    ts.solverPreConduct = argFloat(argc, argv, "--ts-solver-pre-conduct", ts.solverPreConduct);
    ts.upperMidSplitHz = argFloat(argc, argv, "--ts-upper-mid-split", ts.upperMidSplitHz);
    ts.upperBlendLo = argFloat(argc, argv, "--ts-upper-blend-lo", ts.upperBlendLo);
    ts.upperBlendHi = argFloat(argc, argv, "--ts-upper-blend-hi", ts.upperBlendHi);
    ts.upperAirTrimLo = argFloat(argc, argv, "--ts-upper-air-trim-lo", ts.upperAirTrimLo);
    ts.upperAirTrimHi = argFloat(argc, argv, "--ts-upper-air-trim-hi", ts.upperAirTrimHi);
    ts.bodyFeedbackHi = argFloat(argc, argv, "--ts-body-feedback-hi", ts.bodyFeedbackHi);
    ts.bodyHardnessHi = argFloat(argc, argv, "--ts-body-hardness-hi", ts.bodyHardnessHi);
    ts.bodyAsymmetryLo = argFloat(argc, argv, "--ts-body-asymmetry-lo", ts.bodyAsymmetryLo);
    ts.bodyAsymmetryHi = argFloat(argc, argv, "--ts-body-asymmetry-hi", ts.bodyAsymmetryHi);
    ts.upperFeedbackHi = argFloat(argc, argv, "--ts-upper-feedback-hi", ts.upperFeedbackHi);
    ts.upperHardnessHi = argFloat(argc, argv, "--ts-upper-hardness-hi", ts.upperHardnessHi);
    ts.upperAsymmetryScale = argFloat(argc, argv, "--ts-upper-asymmetry-scale", ts.upperAsymmetryScale);
    ts.upperDynamicFeedback = argFloat(argc, argv, "--ts-upper-dynamic-feedback", ts.upperDynamicFeedback);
    ts.upperMidDynamicFeedback = argFloat(argc, argv, "--ts-upper-mid-dynamic-feedback", ts.upperMidDynamicFeedback);
    ts.upperAirDynamicFeedback = argFloat(argc, argv, "--ts-upper-air-dynamic-feedback", ts.upperAirDynamicFeedback);
}

void applyKlonCoreOverride(int argc, char** argv)
{
    auto& klon = SatEngine::OverdriveVoicing::mutableKlonCoreForAnalysis();
    klon.driveScale = argFloat(argc, argv, "--klon-drive-scale", klon.driveScale);
    klon.inputGainDb = argFloat(argc, argv, "--klon-input-gain-db", klon.inputGainDb);
    klon.driveGainScale = argFloat(argc, argv, "--klon-drive-gain-scale", klon.driveGainScale);
    klon.driveGainMaxScale = argFloat(argc, argv, "--klon-drive-gain-max-scale", klon.driveGainMaxScale);
    klon.diodeHeadroomScale = argFloat(argc, argv, "--klon-diode-headroom-scale", klon.diodeHeadroomScale);
    klon.softBlendScale = argFloat(argc, argv, "--klon-soft-blend-scale", klon.softBlendScale);
    klon.cleanAmountScale = argFloat(argc, argv, "--klon-clean-amount-scale", klon.cleanAmountScale);
    klon.dirtyLowMixScale = argFloat(argc, argv, "--klon-dirty-low-mix-scale", klon.dirtyLowMixScale);
    klon.dirtyToneOffset = argFloat(argc, argv, "--klon-dirty-tone-offset", klon.dirtyToneOffset);
    klon.postAsymScale = argFloat(argc, argv, "--klon-post-asym-scale", klon.postAsymScale);
    klon.cleanFreqScale = argFloat(argc, argv, "--klon-clean-freq-scale", klon.cleanFreqScale);
    klon.dirtyLowFreqScale = argFloat(argc, argv, "--klon-dirty-low-freq-scale", klon.dirtyLowFreqScale);
    klon.dirtyFreqScale = argFloat(argc, argv, "--klon-dirty-freq-scale", klon.dirtyFreqScale);
    klon.dirtyAmountScale = argFloat(argc, argv, "--klon-dirty-amount-scale", klon.dirtyAmountScale);
}

void printUsage()
{
    std::cerr << "Usage: SatOverdriveRender --in dry.wav --out sat.wav [--model ts808|klon] [--raw 0|1] [--drive 1] [--type 0] [--knee 0] [--input-db 0] [--output-db 0] [--series 1] [--ts-cascade-csv file.csv] [--ts-drive-scale n ...] [--klon-drive-scale n --klon-input-gain-db db ...]\n";
    std::cerr << "       SatOverdriveRender --batch-json jobs.json\n";
}

std::string readTextFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open batch JSON: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void skipWs(const std::string& s, size_t& i)
{
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
}

std::string parseJsonString(const std::string& s, size_t& i)
{
    if (i >= s.size() || s[i] != '"')
        throw std::runtime_error("expected JSON string");
    ++i;
    std::string out;
    while (i < s.size())
    {
        const char c = s[i++];
        if (c == '"')
            return out;
        if (c == '\\' && i < s.size())
        {
            const char e = s[i++];
            if (e == 'n') out.push_back('\n');
            else if (e == 'r') out.push_back('\r');
            else if (e == 't') out.push_back('\t');
            else out.push_back(e);
        }
        else
        {
            out.push_back(c);
        }
    }
    throw std::runtime_error("unterminated JSON string");
}

std::string parseJsonScalar(const std::string& s, size_t& i)
{
    skipWs(s, i);
    if (i < s.size() && s[i] == '"')
        return parseJsonString(s, i);
    const size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}')
        ++i;
    std::string value = trim(s.substr(start, i - start));
    if (value == "true") return "1";
    if (value == "false") return "0";
    if (value == "null") return "";
    return value;
}

std::vector<std::map<std::string, std::string>> parseBatchJobs(const std::string& path)
{
    const std::string s = readTextFile(path);
    size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '[')
        throw std::runtime_error("batch JSON must be an array of flat objects");
    ++i;
    std::vector<std::map<std::string, std::string>> jobs;
    while (true)
    {
        skipWs(s, i);
        if (i < s.size() && s[i] == ']')
            break;
        if (i >= s.size() || s[i] != '{')
            throw std::runtime_error("expected JSON object in batch array");
        ++i;
        std::map<std::string, std::string> job;
        while (true)
        {
            skipWs(s, i);
            if (i < s.size() && s[i] == '}')
            {
                ++i;
                break;
            }
            const std::string key = parseJsonString(s, i);
            skipWs(s, i);
            if (i >= s.size() || s[i] != ':')
                throw std::runtime_error("expected ':' in batch JSON object");
            ++i;
            job[key] = parseJsonScalar(s, i);
            skipWs(s, i);
            if (i < s.size() && s[i] == ',')
            {
                ++i;
                continue;
            }
            if (i < s.size() && s[i] == '}')
            {
                ++i;
                break;
            }
            throw std::runtime_error("expected ',' or '}' in batch JSON object");
        }
        jobs.push_back(std::move(job));
        skipWs(s, i);
        if (i < s.size() && s[i] == ',')
        {
            ++i;
            continue;
        }
        if (i < s.size() && s[i] == ']')
            break;
    }
    return jobs;
}

std::vector<std::string> argvFromJob(const std::map<std::string, std::string>& job)
{
    static const std::map<std::string, std::string> keyToFlag = {
        {"in", "--in"}, {"out", "--out"}, {"raw", "--raw"}, {"drive", "--drive"},
        {"type", "--type"}, {"knee", "--knee"}, {"char", "--char"}, {"bias", "--bias"},
        {"react", "--react"}, {"detail", "--detail"}, {"inst", "--inst"}, {"series", "--series"},
        {"input_db", "--input-db"}, {"output_db", "--output-db"}, {"input-db", "--input-db"}, {"output-db", "--output-db"},
        {"block_size", "--block-size"}, {"block-size", "--block-size"}, {"ts_cascade_csv", "--ts-cascade-csv"}, {"ts-cascade-csv", "--ts-cascade-csv"},
        {"ts_drive_scale", "--ts-drive-scale"}, {"ts-drive-scale", "--ts-drive-scale"},
        {"ts_drive_headroom", "--ts-drive-headroom"}, {"ts-drive-headroom", "--ts-drive-headroom"},
        {"ts_input_gain_db", "--ts-input-gain-db"}, {"ts-input-gain-db", "--ts-input-gain-db"},
        {"ts_loop_drive_max", "--ts-loop-drive-max"}, {"ts-loop-drive-max", "--ts-loop-drive-max"},
        {"ts_loop_capped_gain", "--ts-loop-capped-gain"}, {"ts-loop-capped-gain", "--ts-loop-capped-gain"},
        {"ts_air_gain", "--ts-air-gain"}, {"ts-air-gain", "--ts-air-gain"},
        {"ts_solver_knee_start", "--ts-solver-knee-start"}, {"ts-solver-knee-start", "--ts-solver-knee-start"},
        {"ts_solver_pre_conduct", "--ts-solver-pre-conduct"}, {"ts-solver-pre-conduct", "--ts-solver-pre-conduct"},
        {"ts_upper_mid_split", "--ts-upper-mid-split"}, {"ts-upper-mid-split", "--ts-upper-mid-split"},
        {"ts_upper_blend_lo", "--ts-upper-blend-lo"}, {"ts-upper-blend-lo", "--ts-upper-blend-lo"},
        {"ts_upper_blend_hi", "--ts-upper-blend-hi"}, {"ts-upper-blend-hi", "--ts-upper-blend-hi"},
        {"ts_upper_air_trim_lo", "--ts-upper-air-trim-lo"}, {"ts-upper-air-trim-lo", "--ts-upper-air-trim-lo"},
        {"ts_upper_air_trim_hi", "--ts-upper-air-trim-hi"}, {"ts-upper-air-trim-hi", "--ts-upper-air-trim-hi"},
        {"ts_body_feedback_hi", "--ts-body-feedback-hi"}, {"ts-body-feedback-hi", "--ts-body-feedback-hi"},
        {"ts_body_hardness_hi", "--ts-body-hardness-hi"}, {"ts-body-hardness-hi", "--ts-body-hardness-hi"},
        {"ts_body_asymmetry_lo", "--ts-body-asymmetry-lo"}, {"ts-body-asymmetry-lo", "--ts-body-asymmetry-lo"},
        {"ts_body_asymmetry_hi", "--ts-body-asymmetry-hi"}, {"ts-body-asymmetry-hi", "--ts-body-asymmetry-hi"},
        {"ts_upper_feedback_hi", "--ts-upper-feedback-hi"}, {"ts-upper-feedback-hi", "--ts-upper-feedback-hi"},
        {"ts_upper_hardness_hi", "--ts-upper-hardness-hi"}, {"ts-upper-hardness-hi", "--ts-upper-hardness-hi"},
        {"ts_upper_asymmetry_scale", "--ts-upper-asymmetry-scale"}, {"ts-upper-asymmetry-scale", "--ts-upper-asymmetry-scale"},
        {"ts_upper_dynamic_feedback", "--ts-upper-dynamic-feedback"}, {"ts-upper-dynamic-feedback", "--ts-upper-dynamic-feedback"},
        {"ts_upper_mid_dynamic_feedback", "--ts-upper-mid-dynamic-feedback"}, {"ts-upper-mid-dynamic-feedback", "--ts-upper-mid-dynamic-feedback"},
        {"ts_upper_air_dynamic_feedback", "--ts-upper-air-dynamic-feedback"}, {"ts-upper-air-dynamic-feedback", "--ts-upper-air-dynamic-feedback"},
        {"klon_drive_scale", "--klon-drive-scale"}, {"klon-drive-scale", "--klon-drive-scale"},
        {"klon_input_gain_db", "--klon-input-gain-db"}, {"klon-input-gain-db", "--klon-input-gain-db"},
        {"klon_drive_gain_scale", "--klon-drive-gain-scale"}, {"klon-drive-gain-scale", "--klon-drive-gain-scale"},
        {"klon_drive_gain_max_scale", "--klon-drive-gain-max-scale"}, {"klon-drive-gain-max-scale", "--klon-drive-gain-max-scale"},
        {"klon_diode_headroom_scale", "--klon-diode-headroom-scale"}, {"klon-diode-headroom-scale", "--klon-diode-headroom-scale"},
        {"klon_soft_blend_scale", "--klon-soft-blend-scale"}, {"klon-soft-blend-scale", "--klon-soft-blend-scale"},
        {"klon_clean_amount_scale", "--klon-clean-amount-scale"}, {"klon-clean-amount-scale", "--klon-clean-amount-scale"},
        {"klon_dirty_low_mix_scale", "--klon-dirty-low-mix-scale"}, {"klon-dirty-low-mix-scale", "--klon-dirty-low-mix-scale"},
        {"klon_dirty_tone_offset", "--klon-dirty-tone-offset"}, {"klon-dirty-tone-offset", "--klon-dirty-tone-offset"},
        {"klon_post_asym_scale", "--klon-post-asym-scale"}, {"klon-post-asym-scale", "--klon-post-asym-scale"},
        {"klon_clean_freq_scale", "--klon-clean-freq-scale"}, {"klon-clean-freq-scale", "--klon-clean-freq-scale"},
        {"klon_dirty_low_freq_scale", "--klon-dirty-low-freq-scale"}, {"klon-dirty-low-freq-scale", "--klon-dirty-low-freq-scale"},
        {"klon_dirty_freq_scale", "--klon-dirty-freq-scale"}, {"klon-dirty-freq-scale", "--klon-dirty-freq-scale"},
        {"klon_dirty_amount_scale", "--klon-dirty-amount-scale"}, {"klon-dirty-amount-scale", "--klon-dirty-amount-scale"},
    };
    std::vector<std::string> args { "SatOverdriveRender" };
    for (const auto& [key, value] : job)
    {
        if (value.empty())
            continue;
        std::string flag;
        if (key.rfind("--", 0) == 0)
            flag = key;
        else
        {
            auto it = keyToFlag.find(key);
            if (it == keyToFlag.end())
                continue;
            flag = it->second;
        }
        args.push_back(flag);
        args.push_back(value);
    }
    return args;
}
}

int renderFromArgs(int argc, char** argv)
{
    try
    {
        if (hasArg(argc, argv, "--help")) { printUsage(); return 0; }
        const std::string inPath = argValue(argc, argv, "--in");
        const std::string outPath = argValue(argc, argv, "--out");
        if (inPath.empty() || outPath.empty()) { printUsage(); return 2; }

        const AudioFile& input = readWav(inPath);
        if (input.channels < 1 || input.channels > 2)
            throw std::runtime_error("only mono/stereo WAV is supported");

        const int channels = input.channels;
        const size_t frames = input.samples.size() / size_t(channels);
        std::vector<float> left(frames), right(frames);
        for (size_t i = 0; i < frames; ++i)
        {
            left[i] = input.samples[i * channels];
            right[i] = channels > 1 ? input.samples[i * channels + 1] : left[i];
        }

        const float inputGain = dbToGain(argFloat(argc, argv, "--input-db", 0.0f));
        const float outputGain = dbToGain(argFloat(argc, argv, "--output-db", 0.0f));
        std::string renderModel = argValue(argc, argv, "--model");
        if (renderModel.empty())
            renderModel = argFloat(argc, argv, "--type", 0.0f) > 0.5f ? "klon" : "ts808";
        const bool renderKlon = renderModel == "klon" || renderModel == "overdrive-b" || renderModel == "overdriveb";
        for (size_t i = 0; i < frames; ++i)
        {
            left[i] *= inputGain;
            right[i] *= inputGain;
        }

#if SAT_TS808_RUNTIME_TUNING
        SatEngine::OverdriveVoicing::resetTs808RuntimeTuningForAnalysis();
#endif
        applyTs808CoreOverride(argc, argv);
        applyKlonCoreOverride(argc, argv);
        applyTs808CascadeCsv(argValue(argc, argv, "--ts-cascade-csv"), renderModel);

        auto statePtr = std::make_unique<SatEngine::State>();
        auto& state = *statePtr;
        state.reset();
        const float drive = argFloat(argc, argv, "--drive", 1.0f);
        const float character = argFloat(argc, argv, "--char", argFloat(argc, argv, "--knee", 0.0f));
        const float type = argFloat(argc, argv, "--type", renderKlon ? 1.0f : 0.0f);
        const float bias = argFloat(argc, argv, "--bias", 0.0f);
        const float react = argFloat(argc, argv, "--react", 0.0f);
        const float detail = argFloat(argc, argv, "--detail", 0.0f);
        const float instability = argFloat(argc, argv, "--inst", 0.0f);
        const bool raw = argInt(argc, argv, "--raw", 0) != 0;
        const int series = argInt(argc, argv, "--series", 1);
        const int blockSize = std::max(1, argInt(argc, argv, "--block-size", 1024));
        const int channelsToProcess = channels > 1 ? 2 : 1;

        // Match the plugin signal path: the Overdrive TS/Klon post EQ runs
        // outside the core/oversampling stage, then is applied once here.
        state.deferOverdriveAPostEq = !raw && !renderKlon;
        state.deferFullKlonPostEq = !raw && renderKlon;

        for (size_t pos = 0; pos < frames; pos += size_t(blockSize))
        {
            const int n = int(std::min<size_t>(size_t(blockSize), frames - pos));
            SatEngine::processBlock(state, left.data() + pos, right.data() + pos, n,
                                    renderKlon ? SatEngine::Model::OverdriveB : SatEngine::Model::OverdriveA,
                                    drive, character, type, bias, react, detail, instability,
                                    float(input.sampleRate), series, true, raw, nullptr, channelsToProcess);
            if (!raw && !renderKlon)
                SatEngine::processDeferredOverdriveAPostEq(state, left.data() + pos, right.data() + pos, n,
                                                               drive, state.sMod, series, float(input.sampleRate), raw);
            else if (!raw && renderKlon)
                SatEngine::processDeferredFullKlonPostEq(state, left.data() + pos, right.data() + pos, n,
                                                         drive, state.sMod, series, float(input.sampleRate), raw);
        }

        if (left.size() != frames || right.size() != frames)
            throw std::runtime_error("internal render buffer size changed unexpectedly");

        AudioFile output;
        output.sampleRate = input.sampleRate;
        output.channels = channels;
        output.samples.resize(frames * size_t(channels));
        for (size_t i = 0; i < frames; ++i)
        {
            output.samples[i * channels] = left[i] * outputGain;
            if (channels > 1)
                output.samples[i * channels + 1] = right[i] * outputGain;
        }
        writeFloatWav(outPath, output);
        std::cout << "Wrote " << outPath << "\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "SatOverdriveRender error: " << e.what() << "\n";
        return 1;
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (hasArg(argc, argv, "--batch-json"))
        {
            const std::string batchPath = argValue(argc, argv, "--batch-json");
            if (batchPath.empty())
            {
                printUsage();
                return 2;
            }
            const auto jobs = parseBatchJobs(batchPath);
            int index = 0;
            for (const auto& job : jobs)
            {
                auto argStrings = argvFromJob(job);
                std::vector<char*> jobArgv;
                jobArgv.reserve(argStrings.size());
                for (auto& s : argStrings)
                    jobArgv.push_back(s.data());
                std::cout << "Batch job " << (index + 1) << "/" << jobs.size() << "\n";
                const int rc = renderFromArgs(static_cast<int>(jobArgv.size()), jobArgv.data());
                if (rc != 0)
                    return rc;
                ++index;
            }
            return 0;
        }
        return renderFromArgs(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "SatOverdriveRender batch error: " << e.what() << "\n";
        return 1;
    }
}
