/*
    MODEL-CLIPONIZER — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.h

    HTML/CSS UI через juce::WebBrowserComponent (JUCE 8).

    Весь малюнок (ручки, осцилоскоп, clip-shaper, LED) тепер живе у
    WebUI/ui.html + WebUI/style.css + WebUI/ui.js. C++ відповідає лише за:
      1) віддачу цих файлів у браузер (resource provider),
      2) прийом команд з JS (зміна параметрів, вибір такту осцилоскопа),
      3) ~30 Гц пуш стану (значення параметрів, дані хвилі, піки, BPM) у JS.
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <limits>
#include <vector>
#include <cstdint>
#include <optional>
#include <array>

class ClipOnizerAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit ClipOnizerAudioProcessorEditor (ClipOnizerAudioProcessor&);
    ~ClipOnizerAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Публічні, бо викликаються з лямбд у Options (побудова webView
    // відбувається до завершення конструктора, тож методи мають бути
    // доступні одразу).
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);

    juce::var jsSetParameter    (const juce::Array<juce::var>& args);
    juce::var jsBeginGesture    (const juce::Array<juce::var>& args);
    juce::var jsEndGesture      (const juce::Array<juce::var>& args);
    juce::var jsSetTimeDivision (const juce::Array<juce::var>& args);
    juce::var jsGetInitialState (const juce::Array<juce::var>& args);
    juce::var jsUiReady         (const juce::Array<juce::var>& args);
    juce::var jsUiShown         (const juce::Array<juce::var>& args);

    // Показує WebView (до цього видно темний колір з paint()) і запускає fade-in.
    void showWebView();

    // Скільки "колонок" (min/max пар) хвилі в одному такті осцилоскопа.
    // JS сам розтягує їх на реальну ширину canvas.
    static constexpr int waveformBuckets = 600;

private:
    // Хвиля одного такту, вже зведена до waveformBuckets колонок min/max.
    // Оновлюється інкрементально — лише колонки, куди прийшли нові дані.
    struct DisplayWave
    {
        std::array<float, waveformBuckets> minV {}, maxV {}, minOut {}, maxOut {};
        int filled = 0;         // заповнені колонки [0, filled)
        int64_t samples = 0;    // семплів такту, що вже надійшли

        void clear() noexcept;
        void addBucket (int column, const ScopeBucket& b) noexcept;
    };

    void timerCallback() override;

    void setTimeDivision (float bars) noexcept;
    void captureNewSamples();
    void pushStateToBrowser();

    double expectedBlockSamples (float bars) const noexcept;
    void addBuckets (int64_t from, int64_t to, int64_t blockStart, double expected);
    void startNewCurrentBlock (int64_t blockId, int64_t blockStart);
    static void resampleWave (DisplayWave& wave, double ratio);

    // Плоский масив цілих [min,max,min,max,...] (значення x1000) для колонок [from, to).
    static void appendWaveformJson (juce::String& json, const DisplayWave& wave,
                                    bool outField, int from, int to);

    ClipOnizerAudioProcessor& audioProcessor;

    // JS повідомляє (uiReady), що сторінка ініціалізована. До цього
    // не пушимо стан — інакше скрипти накопичуються в черзі WebView2
    // і гальмують перше відображення.
    bool uiReady = false;

    // -------------------------------------------------------------------
    // Стан захоплення осцилоскопа — перенесено 1:1 зі старого
    // OscilloscopeComponent, лише без відмальовки (вона тепер у ui.js).
    DisplayWave current;
    DisplayWave previous;
    int64_t lastCapturedCounter = 0;    // у бакетах процесора
    int64_t currentBlockId    = std::numeric_limits<int64_t>::min();
    int64_t currentBlockStart = 0;      // у бакетах процесора
    float barsOnScreen = 1.0f;

    // -------------------------------------------------------------------
    // Економія трафіку C++ -> JS
    bool previousDirty = true;          // previous змінився -> відправити
    int  previousSerial = 0;            // лічильник версій previous для JS
    int  dirtyFrom = 0;                 // перша змінена колонка current з минулого пушу
    int  lastSentFilled = 0;            // скільки колонок current вже має JS
    juce::String lastPushedMeta;        // параметри/піки минулого пушу

    // ВАЖЛИВО: webView оголошено ОСТАННІМ полем. Його Options будуються в
    // member-init-list і вже звертаються до стану вище (getInitialState
    // передається як initialisationData), тож усі поля мають бути готові.
    // До того ж webView знищується першим — лямбди з посиланням на *this
    // гарантовано не переживуть решту полів.
    juce::WebBrowserComponent webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessorEditor)
};
