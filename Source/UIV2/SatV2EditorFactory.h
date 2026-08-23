#pragma once

class SATTRAudioProcessor;
namespace juce { class AudioProcessorEditor; }

namespace SATTR::UIV2
{
juce::AudioProcessorEditor* createEditor(SATTRAudioProcessor& processor);
}
