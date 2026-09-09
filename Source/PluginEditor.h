/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.h
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <limits>
#include <vector>
#include <cstdint>

namespace ClipOnizerColours
{
    const juce::Colour panelDark     { 0xff2b2a28 };
    const juce::Colour panelLight    { 0xff3d3b37 };
    const juce::Colour metalEdge     { 0xff5a564e };
    const juce::Colour scopeGlassBg  { 0xff0c0f0c };
    const juce::Colour scopeGrid     { 0xff2e3a2e };
    const juce::Colour traceNormal   { 0xff8fe3a0 };
    const juce::Colour traceSoftClip { 0xffffb347 };
    const juce::Colour traceHardClip { 0xffff4d4d };
    const juce::Colour thresholdLine { 0xffe0d9c0 };
    const juce::Colour textAmber     { 0xffe8c37a };
    const juce::Colour ledOff        { 0xff1c2a1c };
}

class ClipOnizerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ClipOnizerLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;
};

class ClipShaperComponent : public juce::Component,
                            private juce::Timer
{
public:
    explicit ClipShaperComponent (ClipOnizerAudioProcessor& p);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }

    ClipOnizerAudioProcessor& processor;
    float liveDotSmoothed = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipShaperComponent)
};

class OscilloscopeComponent : public juce::Component,
                              private juce::Timer
{
public:
    explicit OscilloscopeComponent (ClipOnizerAudioProcessor& p);

    void paint (juce::Graphics&) override;

    void setTimeDivision (float bars) noexcept;
    void setVerticalZoom (float zoom) noexcept;

private:
    void captureNewSamples();
    void timerCallback() override
    {
        captureNewSamples();
        repaint();
    }

    ClipOnizerAudioProcessor& processor;

    float barsOnScreen = 1.0f;
    float verticalZoom = 1.0f;

    std::vector<ScopeSample> blockSamples;

    // Absolute counter of the next sample to copy.
    int64_t lastCapturedCounter = 0;

    int64_t currentBlockId =
        std::numeric_limits<int64_t>::min();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscilloscopeComponent)
};

class ClipIndicatorComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit ClipIndicatorComponent (ClipOnizerAudioProcessor& p);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }

    ClipOnizerAudioProcessor& processor;
    float smoothedLevel = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipIndicatorComponent)
};

class ClipOnizerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                       private juce::Timer
{
public:
    explicit ClipOnizerAudioProcessorEditor (ClipOnizerAudioProcessor&);
    ~ClipOnizerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    ClipOnizerAudioProcessor& audioProcessor;
    ClipOnizerLookAndFeel lookAndFeel;

    juce::Slider inputSlider, outputSlider, thresholdSlider, softnessSlider;
    juce::Label inputLabel, outputLabel, thresholdLabel, softnessLabel;
    juce::TextButton deltaButton { "DELTA" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        inputAttachment, outputAttachment, thresholdAttachment, softnessAttachment;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        deltaAttachment;

    ClipShaperComponent clipShaper;
    OscilloscopeComponent oscilloscope;
    ClipIndicatorComponent clipIndicator;

    juce::TextButton timeScaleButtons[6];

    static constexpr float timeScaleValues[6] =
        { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };

    void timeScaleButtonClicked (int index);

    juce::Label bpmLabel;
    juce::Slider verticalZoomSlider;

    juce::Label modelLabel, subtitleLabel, titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessorEditor)
};
