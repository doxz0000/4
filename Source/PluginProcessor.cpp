/*
    MODEL-CLIPONIZER — DR.DOC SOUNDLAB-EQUIPMENT
    PluginProcessor.cpp

    Без змін відносно оригіналу.
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

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        idSaturator, "Saturator",
        juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));

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

    saturatorSmoothed.reset (sampleRate, 0.02);
    satThresholdSmoothed.reset (sampleRate, 0.02);

    saturatorSmoothed.setCurrentAndTargetValue (
        apvts.getRawParameterValue (idSaturator)->load() / 100.0f);

    satThresholdSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idThreshold)->load()));

    saturator.prepare (sampleRate);

    clipEnvelope = 0.0f;
    clipHoldSamplesLeft = 0;
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;

    livePeakPosEnvelope = 0.0f;
    livePeakNegEnvelope = 0.0f;
    livePeakPosHoldSamples = 0;
    livePeakNegHoldSamples = 0;
    scopeLivePeakPos.store (0.0f, std::memory_order_release);
    scopeLivePeakNeg.store (0.0f, std::memory_order_release);

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

    const float saturatorPct = apvts.getRawParameterValue (idSaturator)->load();
    saturatorSmoothed.setTargetValue (juce::jlimit (0.0f, 1.0f, saturatorPct / 100.0f));
    satThresholdSmoothed.setTargetValue (thresholdLin);

    // Coefficients computed once per block; recomputed per sample only while smoothing.
    Saturator::Coeffs satCoeffs = Saturator::makeCoeffs (saturatorSmoothed.getCurrentValue());

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



    float blockMaxReduction = 1.0f;   // max |in| / |out| кліпера за блок (1 = без кліпу)
    float blockPeakAbs = 0.0f;      
    float blockPeakSigned = 0.0f;
    float blockPeakPos = 0.0f;   // найбільше позитивне значення в блоці
    float blockPeakNeg = 0.0f;   // найменше (найнегативніше) значення в блоці   

    for (int i = 0; i < numSamples; ++i)
    {
        const float inGain  = inputGainSmoothed.getNextValue();
        const float outGain = outputGainSmoothed.getNextValue();

        const bool satChanging = saturatorSmoothed.isSmoothing()
                              || satThresholdSmoothed.isSmoothing();
        const float satAmount = saturatorSmoothed.getNextValue();
        const float satThreshold = satThresholdSmoothed.getNextValue();

        if (satChanging)
            satCoeffs = Saturator::makeCoeffs (satAmount);

        float scopeValue = 0.0f;
        float scopeClipAmount = 0.0f;
        float scopeOut = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            const float dry = data[i] * inGain;

            // SATURATOR (before the clipper)
            const float sat = saturator.process (ch, dry, satThreshold,
                                                 satCoeffs, satChanging);

            const auto clipResult =
                ClipShaper::process (sat, thresholdLin, knee01);

            const float clipped = clipResult.first;

            // DELTA = whole chain: what saturator + clipper changed.
            data[i] = deltaOn
                    ? dry - clipped
                    : clipped * outGain;

            if (ch == 0)
            {
                // "BEFORE" scope: signal before saturator + clipper.
                // "AFTER" panel: signal after saturator + clipper,
                // or the DELTA (dry - clipped) when Delta monitoring is on.
                // Zone colouring (scopeClipAmount) is still computed from sat,
                // so the BEFORE view shows what will get clipped.
                scopeValue = dry;
                scopeOut = deltaOn ? (dry - clipped) : clipped;

                // Live peaks are pre-saturation: the shaper panel draws
                // the combined saturator+clipper curve.
                 if (dry > blockPeakPos)
                    blockPeakPos = dry;

                if (dry < blockPeakNeg)
                    blockPeakNeg = dry;

                const float absDry = std::abs (dry);

                if (absDry > blockPeakAbs)
                {
                    blockPeakAbs = absDry;
                    blockPeakSigned = dry;
                }

                const float absSat = std::abs (sat);

                if (absSat > thresholdLin)
                {
                    scopeClipAmount = 1.0f;
                }
                else if (knee01 > 0.001f)
                {
                    // Same relative knee as ClipShaper::process
                    const float kneeWidth = juce::jmax (1.0e-6f,
                                                        juce::jmin (1.0f, knee01) * thresholdLin);
                    const float kneeStart = thresholdLin - kneeWidth;

                    if (absSat > kneeStart)
                    {
                        scopeClipAmount =
                            juce::jlimit (
                                0.001f,
                                0.849f,
                                (absSat - kneeStart)
                                / kneeWidth);
                    }
                }
            }

            // Наскільки кліпер зрізав цей семпл: |до| / |після|.
            if (clipResult.second > 0.0f)
            {
                const float absIn  = std::abs (sat);
                const float absOut = std::abs (clipped);

                if (absOut > 1.0e-6f)
                    blockMaxReduction = juce::jmax (blockMaxReduction, absIn / absOut);
            }
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
            { scopeValue, scopeClipAmount, scopeOut };

        // Release after writing the sample so UI sees the completed entry.
        scopeWriteCounter.store (
            writeCounter + 1,
            std::memory_order_release);
    }

    // CLIP LED: пікове пониження рівня кліпером у dB.
    // Миттєва атака, коротке утримання (щоб UI на 30 Гц не пропустив
    // короткий транзієнт), далі швидкий експоненційний спад.
    // Усе в секундах — не залежить від розміру буфера.
    {
        constexpr double holdSeconds    = 0.15;
        constexpr double releaseSeconds = 0.25;

        const float blockReductionDb =
            juce::Decibels::gainToDecibels (blockMaxReduction, -100.0f);

        if (blockReductionDb >= clipEnvelope)
        {
            clipEnvelope = blockReductionDb;
            clipHoldSamplesLeft = static_cast<int> (holdSeconds * currentSampleRate);
        }
        else if (clipHoldSamplesLeft > 0)
        {
            clipHoldSamplesLeft -= numSamples;
        }
        else
        {
            const float coeff = static_cast<float> (
                std::exp (-static_cast<double> (numSamples)
                          / (releaseSeconds * currentSampleRate)));

            clipEnvelope = blockReductionDb + (clipEnvelope - blockReductionDb) * coeff;

            if (clipEnvelope < 0.01f)
                clipEnvelope = 0.0f;
        }
    }

    clipIndicatorLevel.store (clipEnvelope, std::memory_order_release);

    // --- нове: peak-envelope для точки Clip Shaper ---
    // Миттєва атака, коротке утримання (щоб UI на 30 Гц не пропустив пік),
    // швидкий спад — у секундах, не залежить від розміру буфера.
    // Так точка йде за реальним рівнем і падає в зелену зону між ударами.
    {
        constexpr double liveHoldSeconds    = 0.05;
        constexpr double liveReleaseSeconds = 0.12;

        const int holdSamples = static_cast<int> (liveHoldSeconds * currentSampleRate);
        const float releaseCoeff = static_cast<float> (
            std::exp (-static_cast<double> (numSamples)
                      / (liveReleaseSeconds * currentSampleRate)));

        if (blockPeakPos >= livePeakPosEnvelope)
        {
            livePeakPosEnvelope = blockPeakPos;
            livePeakPosHoldSamples = holdSamples;
        }
        else if (livePeakPosHoldSamples > 0)
        {
            livePeakPosHoldSamples -= numSamples;
        }
        else
        {
            livePeakPosEnvelope = blockPeakPos + (livePeakPosEnvelope - blockPeakPos) * releaseCoeff;
        }
        scopeLivePeakPos.store (livePeakPosEnvelope, std::memory_order_release);

        if (blockPeakNeg <= livePeakNegEnvelope)
        {
            livePeakNegEnvelope = blockPeakNeg;
            livePeakNegHoldSamples = holdSamples;
        }
        else if (livePeakNegHoldSamples > 0)
        {
            livePeakNegHoldSamples -= numSamples;
        }
        else
        {
            livePeakNegEnvelope = blockPeakNeg + (livePeakNegEnvelope - blockPeakNeg) * releaseCoeff;
        }
        scopeLivePeakNeg.store (livePeakNegEnvelope, std::memory_order_release);
    }

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
