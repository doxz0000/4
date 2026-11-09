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

private:
    void timerCallback() override;

    void setTimeDivision (float bars) noexcept;
    void captureNewSamples();
    void pushStateToBrowser();

    // Плоский масив цілих [min,max,min,max,...] (значення x1000).
    // Відправляються лише заповнені бакети — порожній хвіст не передається.
    void appendWaveformJson (juce::String& json,
                             const std::vector<ScopeSample>& samples,
                             int buckets,
                             double referenceTotalSamples,
                             float ScopeSample::* field = &ScopeSample::value) const;

    ClipOnizerAudioProcessor& audioProcessor;

    // Нативний фон, який видно миттєво, поки стартує WebView2 (~0.2–0.8 с).
    juce::Image backgroundImage;

    // JS повідомляє (uiReady), що сторінка ініціалізована. До цього
    // не пушимо стан — інакше скрипти накопичуються в черзі WebView2
    // і гальмують перше відображення.
    bool uiReady = false;

    // -------------------------------------------------------------------
    // Стан захоплення осцилоскопа — перенесено 1:1 зі старого
    // OscilloscopeComponent, лише без відмальовки (вона тепер у ui.js).
    std::vector<ScopeSample> blockSamples;
    std::vector<ScopeSample> previousBlockSamples;
    int64_t lastCapturedCounter = 0;
    int64_t currentBlockId  = std::numeric_limits<int64_t>::min();
    int64_t previousBlockId = std::numeric_limits<int64_t>::min();
    float barsOnScreen = 1.0f;

    // -------------------------------------------------------------------
    // Економія трафіку C++ -> JS
    bool previousDirty = true;          // previousBlockSamples змінився -> відправити
    int  previousSerial = 0;            // лічильник версій previous для JS
    int64_t lastPushedWriteCounter = -1;
    juce::String lastPushedMeta;        // параметри/піки минулого пушу

    // ВАЖЛИВО: webView оголошено ОСТАННІМ полем. Його Options будуються в
    // member-init-list і вже звертаються до стану вище (getInitialState
    // передається як initialisationData), тож усі поля мають бути готові.
    // До того ж webView знищується першим — лямбди з посиланням на *this
    // гарантовано не переживуть решту полів.
    juce::WebBrowserComponent webView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessorEditor)
};
