/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    CLIP-TO-ZERO / No Latency Clipper
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <utility>
#include <cstdint>

struct ScopeSample
{
    float value = 0.0f;
    float clipAmount = 0.0f;
};

class ClipShaper
{
public:
    static std::pair<float, float> process (float x, float thresholdLin, float knee01) noexcept
    {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float absX = std::abs (x);

        if (knee01 < 0.001f)
        {
            if (absX <= thresholdLin)
                return { x, 0.0f };

            return { sign * thresholdLin, 1.0f };
        }

        const float kneeWidth = juce::jmax (0.0005f, knee01);
        const float kneeStart = juce::jmax (0.0f, thresholdLin - kneeWidth);

        if (absX <= kneeStart)
            return { x, 0.0f };

        const float overshoot = absX - kneeStart;
        const float shaped = kneeWidth * std::tanh (overshoot / kneeWidth);
        const float outAbs = kneeStart + shaped;
        const float clipAmount = juce::jlimit (0.0f, 1.0f, shaped / kneeWidth);

        return { sign * outAbs, clipAmount };
    }
};

class ClipOnizerAudioProcessor : public juce::AudioProcessor
{
public:
    ClipOnizerAudioProcessor();
    ~ClipOnizerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static constexpr const char* idInput     = "input";
    static constexpr const char* idOutput    = "output";
    static constexpr const char* idThreshold = "threshold";
    static constexpr const char* idSoftness  = "softness";
    static constexpr const char* idDelta     = "delta";

    // -------------------------------------------------------------------------
    // OSCILLOSCOPE DATA
    //
    // Absolute counter is intentionally used instead of a modulo-only write
    // position. A modulo position cannot tell whether the ring buffer wrapped.
    static constexpr int scopeBufferSize = 1 << 20; // 1,048,576 samples
    std::array<ScopeSample, scopeBufferSize> scopeBuffer {};

    std::atomic<int64_t> scopeWriteCounter { 0 };

    // Current musical block and the absolute counter where it started.
    std::atomic<int64_t> scopeBlockId { std::numeric_limits<int64_t>::min() };
    std::atomic<int64_t> scopeBlockStartCounter { 0 };

    // Current host information, written only from processBlock().
    std::atomic<double> currentBpm { 120.0 };
    std::atomic<double> currentQuarterNotesPerBar { 4.0 };

    // Selected oscilloscope length: 0.25 / 0.5 / 1 / 2 / 4 / 8 bars.
    std::atomic<float> oscilloscopeBars { 1.0f };
    std::atomic<uint32_t> oscilloscopeConfigVersion { 0 };

    // Fallback transport when PPQ is unavailable.
    double fallbackPpq = 0.0;

    std::atomic<float> clipIndicatorLevel { 0.0f };

    void setOscilloscopeBars (float bars) noexcept
    {
        bars = juce::jlimit (0.25f, 8.0f, bars);
        oscilloscopeBars.store (bars, std::memory_order_release);
        oscilloscopeConfigVersion.fetch_add (1, std::memory_order_acq_rel);
    }

    float getOscilloscopeBars() const noexcept
    {
        return oscilloscopeBars.load (std::memory_order_acquire);
    }

    float getThresholdLinear() const noexcept;
    float getSoftnessNormalized() const noexcept;

private:
    juce::LinearSmoothedValue<float> inputGainSmoothed { 1.0f };
    juce::LinearSmoothedValue<float> outputGainSmoothed { 1.0f };

    float clipEnvelope = 0.0f;
    uint32_t lastScopeConfigVersion = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessor)
};
