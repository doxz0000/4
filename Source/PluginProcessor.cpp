/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginProcessor.cpp
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

//==============================================================================
ClipOnizerAudioProcessor::ClipOnizerAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

ClipOnizerAudioProcessor::~ClipOnizerAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout
ClipOnizerAudioProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        idInput, "Input",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        idOutput, "Output",
        juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        idThreshold, "Threshold",
        juce::NormalisableRange<float> (-24.0f, 10.0f, 0.01f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        idSoftness, "Softness",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 20.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

    params.push_back (std::make_unique<juce::AudioParameterBool> (
        idDelta, "Delta", false));

    return { params.begin(), params.end() };
}

//==============================================================================
void ClipOnizerAudioProcessor::prepareToPlay (double sampleRate, int)
{
    inputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.reset (sampleRate, 0.02);

    inputGainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idInput)->load()));

    outputGainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idOutput)->load()));

    clipEnvelope = 0.0f;

    scopeWriteCounter.store (0, std::memory_order_release);
    scopeBlockId.store (std::numeric_limits<int64_t>::min(), std::memory_order_release);
    scopeBlockStartCounter.store (0, std::memory_order_release);

    currentBpm.store (120.0, std::memory_order_release);
    currentQuarterNotesPerBar.store (4.0, std::memory_order_release);

    fallbackPpq = 0.0;
    scopeLoopPreviousPpq = 0.0;
    scopeLoopPreviousIsLooping = false;
    lastScopeConfigVersion =
        oscilloscopeConfigVersion.load (std::memory_order_acquire);

    clipIndicatorLevel.store (0.0f, std::memory_order_release);
}

void ClipOnizerAudioProcessor::releaseResources()
{
}

bool ClipOnizerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono()
        && mainOut != juce::AudioChannelSet::stereo())
        return false;

    return mainOut == layouts.getMainInputChannelSet();
}

//==============================================================================
void ClipOnizerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                             juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    const float inputGainDb  = apvts.getRawParameterValue (idInput)->load();
    const float outputGainDb = apvts.getRawParameterValue (idOutput)->load();
    const float thresholdDb  = apvts.getRawParameterValue (idThreshold)->load();
    const float softnessPct  = apvts.getRawParameterValue (idSoftness)->load();
    const bool deltaOn       = apvts.getRawParameterValue (idDelta)->load() > 0.5f;

    inputGainSmoothed.setTargetValue (
        juce::Decibels::decibelsToGain (inputGainDb));

    outputGainSmoothed.setTargetValue (
        juce::Decibels::decibelsToGain (outputGainDb));

    const float thresholdLin = juce::Decibels::decibelsToGain (thresholdDb);
    const float knee01 = juce::jlimit (0.0f, 1.0f, softnessPct / 100.0f);

    // -------------------------------------------------------------------------
    // Host transport is read ONLY here, from processBlock().
    // The editor never calls getPlayHead().
    double bpm = currentBpm.load (std::memory_order_relaxed);
    double qnPerBar = currentQuarterNotesPerBar.load (std::memory_order_relaxed);

    bool havePpq = false;
    double ppqStart = 0.0;
    bool isLooping = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
  

    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            if (auto hostBpm = position->getBpm())
            {
                bpm = juce::jmax (20.0, *hostBpm);
                currentBpm.store (bpm, std::memory_order_release);
            }

            if (auto ts = position->getTimeSignature())
            {
                const int numerator = juce::jmax (1, ts->numerator);
                const int denominator = juce::jmax (1, ts->denominator);

                qnPerBar = static_cast<double> (numerator)
                         * (4.0 / static_cast<double> (denominator));

                currentQuarterNotesPerBar.store (qnPerBar, std::memory_order_release);
            }

         if (auto ppq = position->getPpqPosition())
            {
                havePpq = true;
                ppqStart = *ppq;
            }

            isLooping = position->getIsLooping();

            if (auto loop = position->getLoopPoints())
            {
                loopStart = loop->ppqStart;
                loopEnd   = loop->ppqEnd;
            }
        }
    }

    const float selectedBars =
        juce::jlimit (0.25f, 8.0f,
                      oscilloscopeBars.load (std::memory_order_acquire));

    const double blockLengthQN =
        juce::jmax (0.25, qnPerBar * static_cast<double> (selectedBars));

    // Changing 1/4, 1/2, 1, 2, 4, 8 BAR forces a clean new scope block.
    const uint32_t configVersion =
        oscilloscopeConfigVersion.load (std::memory_order_acquire);

    if (configVersion != lastScopeConfigVersion)
    {
        scopeBlockId.store (std::numeric_limits<int64_t>::min(),
                            std::memory_order_release);
        scopeBlockStartCounter.store (
            scopeWriteCounter.load (std::memory_order_relaxed),
            std::memory_order_release);

        lastScopeConfigVersion = configVersion;
    }

    const double sampleRate = juce::jmax (1.0, getSampleRate());
    const double ppqPerSample = bpm / (60.0 * sampleRate);

    // -------------------------------------------------------------------------
    // We determine the musical block PER SAMPLE, not once per audio callback.
    // This fixes the common case where a 512-sample callback crosses a bar line.
   auto getBlockIdAndPosition = [&] (double ppq,
                                  int64_t& blockId,
                                  double& positionInBlock) noexcept
{
    double musicalPpq = ppq;

    if (isLooping && loopEnd > loopStart)
    {
        const double loopLength = loopEnd - loopStart;

        double relative = ppq - loopStart;

        // Detect that the DAW has jumped back to the beginning
        // of the loop.
       if (scopeLoopPreviousIsLooping
            && scopeLoopPreviousPpq >= loopStart
            && scopeLoopPreviousPpq < loopEnd
            && ppq < scopeLoopPreviousPpq)
        {
            scopeLoopCycle.fetch_add (
                1,
                std::memory_order_acq_rel);
        }

        relative = std::fmod (relative, loopLength);

        if (relative < 0.0)
            relative += loopLength;

        const int64_t loopCycle =
            scopeLoopCycle.load (
                std::memory_order_relaxed);

        // Continuous PPQ position across DAW loop repetitions.
        musicalPpq =
            static_cast<double> (loopCycle) * loopLength
            + relative;

        const double relativeBlock =
            musicalPpq / blockLengthQN;

        blockId =
            static_cast<int64_t> (
                std::floor (relativeBlock));

        positionInBlock =
            musicalPpq
            - static_cast<double> (blockId)
              * blockLengthQN;
    }
    else
    {
        const double relativeBlock =
            musicalPpq / blockLengthQN;

        blockId =
            static_cast<int64_t> (
                std::floor (relativeBlock));

        positionInBlock =
            musicalPpq
            - static_cast<double> (blockId)
              * blockLengthQN;
    }

    scopeLoopPreviousPpq = ppq;
    scopeLoopPreviousIsLooping = isLooping;
};


    float blockMaxClip = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inGain  = inputGainSmoothed.getNextValue();
        const float outGain = outputGainSmoothed.getNextValue();

        float scopeValue = 0.0f;
        float scopeClipAmount = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            const float dry = data[i] * inGain;

            const auto clipResult =
                ClipShaper::process (dry, thresholdLin, knee01);

            const float clipped = clipResult.first;
            const float clipAmount = clipResult.second;

            data[i] = deltaOn
                    ? dry - clipped
                    : clipped * outGain;

            if (ch == 0)
            {
                // IMPORTANT: scope shows PRE-CLIP signal.
                scopeValue = dry;

                const float absDry = std::abs (dry);

                if (absDry > thresholdLin)
                {
                    scopeClipAmount = 1.0f;
                }
                else if (knee01 > 0.001f)
                {
                    const float kneeWidth = juce::jmax (0.0005f, knee01);
                    const float kneeStart = juce::jmax (0.0f,
                                                        thresholdLin - kneeWidth);

                    if (absDry > kneeStart)
                    {
                        scopeClipAmount =
                            juce::jlimit (
                                0.001f,
                                0.849f,
                                (absDry - kneeStart)
                                / juce::jmax (0.0005f,
                                              thresholdLin - kneeStart));
                    }
                }
            }

            blockMaxClip = juce::jmax (blockMaxClip, clipAmount);
        }

        // Determine exact block for this sample.
        double samplePpq = 0.0;

        if (havePpq)
        {
            samplePpq = ppqStart + static_cast<double> (i) * ppqPerSample;
        }
        else
        {
            samplePpq = fallbackPpq;
            fallbackPpq += ppqPerSample;
        }

        int64_t blockId = 0;
        double positionInBlock = 0.0;
        getBlockIdAndPosition (samplePpq, blockId, positionInBlock);

        const int64_t writeCounter =
            scopeWriteCounter.load (std::memory_order_relaxed);

        const int64_t activeBlock =
            scopeBlockId.load (std::memory_order_relaxed);

        if (activeBlock != blockId)
        {
            // The next sample belongs to a new musical block.
            // Start counter points EXACTLY at this sample.
            scopeBlockStartCounter.store (
                writeCounter,
                std::memory_order_release);

            scopeBlockId.store (
                blockId,
                std::memory_order_release);
        }

        const int index =
            static_cast<int> (writeCounter % scopeBufferSize);

        scopeBuffer[static_cast<size_t> (index)] =
            { scopeValue, scopeClipAmount };

        // Release after writing the sample so UI sees the completed entry.
        scopeWriteCounter.store (
            writeCounter + 1,
            std::memory_order_release);
    }

    constexpr float attack = 0.35f;
    constexpr float release = 0.001f;

    const float coeff =
        blockMaxClip > clipEnvelope ? attack : release;

    clipEnvelope += (blockMaxClip - clipEnvelope) * coeff;

    clipIndicatorLevel.store (clipEnvelope, std::memory_order_release);
}

//==============================================================================
float ClipOnizerAudioProcessor::getThresholdLinear() const noexcept
{
    return juce::Decibels::decibelsToGain (
        apvts.getRawParameterValue (idThreshold)->load());
}

float ClipOnizerAudioProcessor::getSoftnessNormalized() const noexcept
{
    return juce::jlimit (
        0.0f,
        1.0f,
        apvts.getRawParameterValue (idSoftness)->load() / 100.0f);
}

//==============================================================================
juce::AudioProcessorEditor* ClipOnizerAudioProcessor::createEditor()
{
    return new ClipOnizerAudioProcessorEditor (*this);
}

//==============================================================================
void ClipOnizerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void ClipOnizerAudioProcessor::setStateInformation (const void* data,
                                                    int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (
        getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClipOnizerAudioProcessor();
}
