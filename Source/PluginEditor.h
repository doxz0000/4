/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.h

    !!! ЦЕ ЕТАП "ЛОГІКА ПРАЦЮЄ" !!!
    Зараз усі елементи намальовані примітивами JUCE (Graphics-фігури,
    градієнти, стандартні Slider/TextButton). Це зроблено свідомо:
      1. щоб усе можна було одразу протестувати з реальним звуком;
      2. щоб потім було легко підмінити відповідні шматки на растрові
         спрайти (наприклад, у ClipOnizerLookAndFeel::drawRotarySlider
         та у фонах ClipShaperComponent / OscilloscopeComponent /
         ClipIndicatorComponent) без зміни логіки взаємодії з DSP.
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <limits>
#include <vector>

//==============================================================================
// Тимчасова "вінтажно-лабораторна" палітра. Коли додаси картинку дизайну,
// значна частина цих кольорів піде саме у прошарки під спрайтами
// (підсвітка, кольори тераси/трасування графіків тощо).
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

//==============================================================================
class ClipOnizerLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ClipOnizerLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                            float sliderPosProportional, float rotaryStartAngle,
                            float rotaryEndAngle, juce::Slider&) override;
};

//==============================================================================
// ЛІВИЙ аналізатор: CLIP SHAPER — Input level -> Output level (transfer function)
class ClipShaperComponent : public juce::Component,
                             private juce::Timer
{
public:
    explicit ClipShaperComponent (ClipOnizerAudioProcessor& p);
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override { repaint(); }

    ClipOnizerAudioProcessor& processor;
    float liveDotSmoothed = 0.0f; // легке згладжування руху точки по кривій

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipShaperComponent)
};

//==============================================================================
// ПРАВИЙ аналізатор: BLOCK-BASED REAL-TIME OSCILLOSCOPE
//
// Осцилограф більше НЕ скролиться постійно.
// Він працює музичними блоками:
//
// 1/4 BAR
// 1/2 BAR
// 1 BAR
// 2 BAR
// 4 BAR
// 8 BAR
//
// Waveform поступово заповнює фіксоване вікно зліва направо.
// Коли поточний музичний блок закінчується —
// починається наступний блок.
//
// Якщо host не передає PPQ, використовується fallback через BPM.
//==============================================================================

class OscilloscopeComponent : public juce::Component,
                               private juce::Timer
{
public:
    explicit OscilloscopeComponent (ClipOnizerAudioProcessor& p);

    void paint (juce::Graphics&) override;

    // Кількість тактів, які повинні бути в одному блоці.
    void setTimeDivision (float bars) noexcept
    {
        barsOnScreen = juce::jmax (0.25f, bars);

        // Новий вибір масштабу = починаємо новий блок.
        currentBlockId = std::numeric_limits<int64_t>::min();
        blockSamples.clear();
        lastCapturedWritePos = processor.scopeWritePos.load (std::memory_order_relaxed);
    }

    // Вертикальний visual-only zoom.
    void setVerticalZoom (float zoom) noexcept
    {
        verticalZoom = zoom;
    }

private:
    //==========================================================================
    struct BlockPosition
    {
        bool valid = false;

        // Номер поточного музичного блоку.
        int64_t blockId = 0;

        // Позиція всередині поточного блоку в quarter notes.
        double positionInBlock = 0.0;

        // Повна довжина блоку в quarter notes.
        double blockLengthInQuarterNotes = 4.0;

        // Поточний BPM.
        double bpm = 120.0;
    };

    // Отримуємо позицію DAW.
    BlockPosition getCurrentBlockPosition();

    // Забираємо нові семпли з processor.scopeBuffer
    // і додаємо їх у поточний блок.
    void captureNewSamples();

    // Очищаємо старий блок при переході на наступний.
    void startNewBlock (int64_t blockId);

    void timerCallback() override
    {
        captureNewSamples();
        repaint();
    }

    ClipOnizerAudioProcessor& processor;

    // Кількість тактів на екрані.
    float barsOnScreen = 1.0f;

    // Visual-only vertical zoom.
    float verticalZoom = 1.0f;

    //==========================================================================
    // Поточний заблокований waveform.
    //
    // ВАЖЛИВО:
    // Тут зберігається вже не ring-buffer вікно,
    // а окремий waveform поточного музичного блоку.
    std::vector<ScopeSample> blockSamples;

    // Остання позиція ring-buffer, яку ми вже забрали.
    int lastCapturedWritePos = 0;

    // ID поточного музичного блоку.
    int64_t currentBlockId = std::numeric_limits<int64_t>::min();

    //==========================================================================
    // Fallback clock, якщо DAW не дає PPQ.
    double fallbackBeatPosition = 0.0;
    double fallbackLastTimeSeconds = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscilloscopeComponent)
};

//==============================================================================
// CLIP-індикатор: "аналогова лампа" з інтенсивністю та кольором залежно
// від того, наскільки часто/сильно/довго відбувається clipping.
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

//==============================================================================
class ClipOnizerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit ClipOnizerAudioProcessorEditor (ClipOnizerAudioProcessor&);
    ~ClipOnizerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override; // оновлення BPM-лейбла

    ClipOnizerAudioProcessor& audioProcessor;
    ClipOnizerLookAndFeel lookAndFeel;

    // Основні ручки
    juce::Slider inputSlider, outputSlider, thresholdSlider, softnessSlider;
    juce::Label  inputLabel, outputLabel, thresholdLabel, softnessLabel;
    juce::TextButton deltaButton { "DELTA" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        inputAttachment, outputAttachment, thresholdAttachment, softnessAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> deltaAttachment;

    // Аналізатори + CLIP-лампа
    ClipShaperComponent   clipShaper;
    OscilloscopeComponent oscilloscope;
    ClipIndicatorComponent clipIndicator;

    // OSCILLOSCOPE TIME SCALE: 1/4 | 1/2 | 1 | 2 | 4 | 8
    juce::TextButton timeScaleButtons[6];
    static constexpr float timeScaleValues[6] = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f };
    void timeScaleButtonClicked (int index);
    juce::Label bpmLabel;

    // VERTICAL OSCILLOSCOPE ZOOM
    juce::Slider verticalZoomSlider;

    // Заголовки/шильдик
    juce::Label modelLabel, subtitleLabel, titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessorEditor)
};
