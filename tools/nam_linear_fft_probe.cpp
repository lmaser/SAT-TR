#include "NAM/linear.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
std::vector<float> makeInput (int count)
{
    std::vector<float> result (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
        result[static_cast<size_t> (i)] = static_cast<float> (0.2 * std::sin (0.013 * i)
                                                               + 0.05 * std::cos (0.071 * i));
    return result;
}

std::vector<float> makeWeights (int count)
{
    std::vector<float> result (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
        result[static_cast<size_t> (i)] = static_cast<float> (std::exp (-0.001 * i)
                                                               * std::sin (0.037 * (i + 1)) * 0.01);
    return result;
}

std::vector<float> process (nam::Linear& model, const std::vector<float>& input)
{
    std::vector<float> output (input.size(), 0.0f);
    NAM_SAMPLE* inPtrs[1];
    NAM_SAMPLE* outPtrs[1];
    constexpr int chunks[] { 1, 17, 64, 255, 3, 512, 31 };
    size_t offset = 0;
    size_t chunkIndex = 0;

    while (offset < input.size())
    {
        const int count = std::min (chunks[chunkIndex % std::size (chunks)],
                                    static_cast<int> (input.size() - offset));
        inPtrs[0] = const_cast<NAM_SAMPLE*> (input.data() + offset);
        outPtrs[0] = output.data() + offset;
        model.process (inPtrs, outPtrs, count);
        offset += static_cast<size_t> (count);
        ++chunkIndex;
    }

    return output;
}
}

int main()
{
    constexpr int receptiveField = 1536;
    const auto weights = makeWeights (receptiveField);
    const auto input = makeInput (4096);

    nam::Linear direct (1, 1, receptiveField, false, weights, 48000.0,
                        nam::LinearImplementation::Direct);
    nam::Linear fft (1, 1, receptiveField, false, weights, 48000.0,
                     nam::LinearImplementation::FFT);
    direct.Reset (48000.0, 512);
    fft.Reset (48000.0, 512);

    const auto directOutput = process (direct, input);
    const auto fftOutput = process (fft, input);
    double maxDifference = 0.0;
    for (size_t i = 0; i < input.size(); ++i)
        maxDifference = std::max (maxDifference, static_cast<double> (std::abs (directOutput[i] - fftOutput[i])));

    std::printf ("direct_active=%d fft_active=%d max_abs_diff=%.12g\n",
                 static_cast<int> (direct.GetActiveImplementation()),
                 static_cast<int> (fft.GetActiveImplementation()), maxDifference);
    return maxDifference < 5.0e-5 ? 0 : 1;
}
