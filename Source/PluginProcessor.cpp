/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginProcessor.cpp
*/

#include "PluginProcessor.h"
#include "PluginEditor.h"

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
juce::AudioProcessorValueTreeState::ParameterLayout ClipOnizerAudioProcessor::createParameterLayout()
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

    // Threshold: "від негативних значень до +10 dB" — беремо робочий діапазон -24..+10 dB.
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
    const double rampSeconds = 0.02; // 20 мс — швидко, але без кліків при русі ручки

    inputGainSmoothed.reset (sampleRate, rampSeconds);
    outputGainSmoothed.reset (sampleRate, rampSeconds);

    inputGainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idInput)->load()));
    outputGainSmoothed.setCurrentAndTargetValue (
        juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idOutput)->load()));

    clipEnvelope = 0.0f;
    clipIndicatorLevel.store (0.0f);
    scopeWritePos.store (0);
}

void ClipOnizerAudioProcessor::releaseResources()
{
}

bool ClipOnizerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    return mainOut == layouts.getMainInputChannelSet();
}

//==============================================================================
void ClipOnizerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // --- Читаємо параметри один раз на блок (окрім gain, який згладжується поseмплово) ---
    const float inputGainDb  = apvts.getRawParameterValue (idInput)->load();
    const float outputGainDb = apvts.getRawParameterValue (idOutput)->load();
    const float thresholdDb  = apvts.getRawParameterValue (idThreshold)->load();
    const float softnessPct  = apvts.getRawParameterValue (idSoftness)->load();
    const bool  deltaOn      = apvts.getRawParameterValue (idDelta)->load() > 0.5f;

    inputGainSmoothed.setTargetValue  (juce::Decibels::decibelsToGain (inputGainDb));
    outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGainDb));

    const float thresholdLin = juce::Decibels::decibelsToGain (thresholdDb);
    const float knee01       = juce::jlimit (0.0f, 1.0f, softnessPct / 100.0f);

    // --- BPM хоста, для синхронізації часової шкали осцилографа в UI ---
    // ВАЖЛИВО: це впливає лише на те, що ми показуємо у UI. Аудіо-сигнал
    // (та, відповідно, і latency) від цього ніяк не залежить.
    // ВИПРАВЛЕННЯ: локальна змінна перейменована з "playHead" на "hostPlayHead",
    // щоб не затіняти член класу AudioProcessor::playHead (усуває warning C4458).
    if (auto* hostPlayHead = getPlayHead())
    {
        if (auto position = hostPlayHead->getPosition())
        {
            if (auto bpm = position->getBpm())
                currentBpm.store (*bpm);
        }
    }

    float blockMaxClip = 0.0f;

    // --- Основний DSP-цикл: sample-outer / channel-inner, ЩОБ gain-smoothing ---
    // рухався рівно один раз на семпл (а не на кожен канал).
    for (int i = 0; i < numSamples; ++i)
    {
        const float inGain  = inputGainSmoothed.getNextValue();
        const float outGain = outputGainSmoothed.getNextValue();

        float scopeValue = 0.0f;
        float scopeClipAmount = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* data = buffer.getWritePointer (ch);

            // Сигнал ПІСЛЯ input gain, ДО clip — потрібен і для clip-логіки,
            // і як опорна точка для обчислення Delta.
            const float dry = data[i] * inGain;

            const auto clipResult = ClipShaper::process (dry, thresholdLin, knee01);
            const float clipped    = clipResult.first;
            const float clipAmount = clipResult.second;

            float outSample;

            if (deltaOn)
            {
                // DELTA = те, що кліпер прибирає з сигналу.
                // Свідомо БЕЗ output gain і без будь-якої "гучносної" компенсації —
                // інакше це просто інший спосіб чути зміну громкості, а не сам clip-артефакт.
                outSample = dry - clipped;
            }
            else
            {
                outSample = clipped * outGain;
            }

            data[i] = outSample;

            if (ch == 0)
            {
                scopeValue = deltaOn ? outSample : clipped;
                scopeClipAmount = clipAmount;
            }

            blockMaxClip = juce::jmax (blockMaxClip, clipAmount);
        }

        const int pos = scopeWritePos.fetch_add (1, std::memory_order_relaxed) % scopeBufferSize;
        scopeBuffer[(size_t) pos] = { scopeValue, scopeClipAmount };
    }

    // --- Envelope-follower для CLIP-лампи ---
    // Швидка атака (лампа миттєво "запалюється"), повільний спад
    // (лампа плавно "тримає" колір — як інерція реальної аналогової лампи,
    // а не миттєве вимкнення CSS-кольору).
    constexpr float attack  = 0.35f;
    constexpr float release = 0.001f;
    const float coeff = blockMaxClip > clipEnvelope ? attack : release;
    clipEnvelope += (blockMaxClip - clipEnvelope) * coeff;
    clipIndicatorLevel.store (clipEnvelope);
}

//==============================================================================
float ClipOnizerAudioProcessor::getThresholdLinear() const noexcept
{
    return juce::Decibels::decibelsToGain (apvts.getRawParameterValue (idThreshold)->load());
}

float ClipOnizerAudioProcessor::getSoftnessNormalized() const noexcept
{
    return juce::jlimit (0.0f, 1.0f, apvts.getRawParameterValue (idSoftness)->load() / 100.0f);
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

void ClipOnizerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

//==============================================================================
// Обов'язкова точка входу для JUCE-плагінів.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ClipOnizerAudioProcessor();
}
