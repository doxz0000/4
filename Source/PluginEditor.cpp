/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.cpp
*/

#include "PluginEditor.h"
#include "BinaryData.h"
#include <cmath>

namespace
{
    // Скільки "колонок" (min/max пар) хвилі відправляти в JS за один тік.
    // JS сам розтягує їх на реальну ширину canvas.
    constexpr int waveformBuckets = 600;

    juce::WebBrowserComponent::Resource makeResource (const void* data, int size, const juce::String& mime)
    {
        auto* bytePtr = static_cast<const std::byte*> (data);
        return { std::vector<std::byte> (bytePtr, bytePtr + size), mime };
    }

    juce::WebBrowserComponent::Options makeWebViewOptions (ClipOnizerAudioProcessorEditor& editor)
    {
        using Options = juce::WebBrowserComponent::Options;

        return Options{}.withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
            .withNativeIntegrationEnabled()
            // JUCE 9: другий аргумент — allowedOrigin (CORS); для релізу не потрібен
            .withResourceProvider ([&editor] (const juce::String& url)
                                    {
                                        return editor.getResource (url);
                                    })
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (juce::File::getSpecialLocation (juce::File::SpecialLocationType::tempDirectory)))
            .withNativeFunction ("setParam",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsSetParameter (args)); })
            .withNativeFunction ("beginGesture",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsBeginGesture (args)); })
            .withNativeFunction ("endGesture",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsEndGesture (args)); })
            .withNativeFunction ("setTimeDivision",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsSetTimeDivision (args)); })
            .withNativeFunction ("getInitialState",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsGetInitialState (args)); });
    }
}

//==============================================================================
ClipOnizerAudioProcessorEditor::ClipOnizerAudioProcessorEditor (ClipOnizerAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      webView (makeWebViewOptions (*this))
{
    addAndMakeVisible (webView);

    lastCapturedCounter = audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);

    // JUCE 9: resource provider обслуговує корінь getResourceProviderRoot()
    // (https://juce.backend). Навігація на будь-який інший домен (напр.
    // http://cliponizer.local) піде в реальну мережу і дасть "Error code: 13".
    webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + "/ui.html");

    // Фіксований стандартний розмір — масштабування/зміну розміру вимкнено.
    // setResizable(false) прибирає перетягування країв і кутовий хендл,
    // а ліміти мін=макс не дають хосту (DAW) змінити розмір програмно.
    setResizable (false, false);
    setResizeLimits (980, 660, 980, 660);
    setSize (980, 660);

    startTimerHz (30);
}

ClipOnizerAudioProcessorEditor::~ClipOnizerAudioProcessorEditor() = default;

void ClipOnizerAudioProcessorEditor::resized()
{
    webView.setBounds (getLocalBounds());
}

//==============================================================================
// RESOURCE PROVIDER — serves WebUI/*.html/css/js (+ the plugin's images)
// straight out of BinaryData, so the finished plugin is self-contained.
//==============================================================================
std::optional<juce::WebBrowserComponent::Resource>
ClipOnizerAudioProcessorEditor::getResource (const juce::String& url)
{
    // JUCE 9: провайдер отримує шлях відносно getResourceProviderRoot(),
    // напр. "/ui.html". Також підтримуємо повні URL (fallback).
    juce::String path = url;

    if (auto queryPos = path.indexOfChar ('?'); queryPos >= 0)
        path = path.substring (0, queryPos);

    // Обрізаємо доменну частину, якщо прийшов повний URL
    if (path.contains ("//"))
        path = path.fromLastOccurrenceOf ("//", false, false);

    if (path.endsWithIgnoreCase (".html") || path.endsWithIgnoreCase (".css") || path.endsWithIgnoreCase (".js"))
    {
        juce::String fileName = path.fromLastOccurrenceOf ("/", false, false);

        const void* data = nullptr; int size = 0; const char* mime = "text/html";

        if (fileName == "ui.html" || fileName.isEmpty() || path.endsWithIgnoreCase ("/"))
        {   data = BinaryData::ui_html;          size = BinaryData::ui_htmlSize;          }
        else if (fileName == "style.css")
        {   data = BinaryData::style_css;        size = BinaryData::style_cssSize;        mime = "text/css"; }
        else if (fileName == "ui.js")
        {   data = BinaryData::ui_js;            size = BinaryData::ui_jsSize;            mime = "text/javascript"; }
        else if (fileName == "juce_interop.js")
        {   data = BinaryData::juce_interop_js;  size = BinaryData::juce_interop_jsSize;  mime = "text/javascript"; }

        if (data != nullptr)
            return makeResource (data, size, mime);
    }

    if (path.endsWithIgnoreCase ("background.png"))
        return makeResource (BinaryData::background_png, BinaryData::background_pngSize, "image/png");

    if (path.endsWithIgnoreCase ("knob.png"))
        return makeResource (BinaryData::knob_png, BinaryData::knob_pngSize, "image/png");

    if (path.endsWithIgnoreCase ("DeltaOn.png"))
        return makeResource (BinaryData::DeltaOn_png, BinaryData::DeltaOn_pngSize, "image/png");

    if (path.endsWithIgnoreCase ("DeltaOff.png"))
        return makeResource (BinaryData::DeltaOff_png, BinaryData::DeltaOff_pngSize, "image/png");

    // Корінь ("/") — index.html
    if (path == "/" || path.isEmpty() || path.endsWithIgnoreCase ("/"))
        return makeResource (BinaryData::ui_html, BinaryData::ui_htmlSize, "text/html");

    return std::nullopt;
}

//==============================================================================
// NATIVE FUNCTIONS — called from JavaScript (window.__JUCE__.backend.invoke)
//==============================================================================
juce::var ClipOnizerAudioProcessorEditor::jsSetParameter (const juce::Array<juce::var>& args)
{
    if (args.size() < 2)
        return false;

    const juce::String paramId = args[0].toString();
    auto* param = audioProcessor.apvts.getParameter (paramId);

    if (param == nullptr)
        return false;

    if (paramId == ClipOnizerAudioProcessor::idDelta)
    {
        param->setValueNotifyingHost (((bool) args[1]) ? 1.0f : 0.0f);
        return true;
    }

    const float value = (float) (double) args[1];
    param->setValueNotifyingHost (param->convertTo0to1 (value));
    return true;
}

juce::var ClipOnizerAudioProcessorEditor::jsBeginGesture (const juce::Array<juce::var>& args)
{
    if (args.isEmpty())
        return false;

    if (auto* param = audioProcessor.apvts.getParameter (args[0].toString()))
        param->beginChangeGesture();

    return true;
}

juce::var ClipOnizerAudioProcessorEditor::jsEndGesture (const juce::Array<juce::var>& args)
{
    if (args.isEmpty())
        return false;

    if (auto* param = audioProcessor.apvts.getParameter (args[0].toString()))
        param->endChangeGesture();

    return true;
}

juce::var ClipOnizerAudioProcessorEditor::jsSetTimeDivision (const juce::Array<juce::var>& args)
{
    if (args.isEmpty())
        return false;

    setTimeDivision ((float) (double) args[0]);
    return true;
}

juce::var ClipOnizerAudioProcessorEditor::jsGetInitialState (const juce::Array<juce::var>&)
{
    auto* obj = new juce::DynamicObject();

    auto describeParam = [this] (const juce::String& id) -> juce::var
    {
        auto* o = new juce::DynamicObject();

        if (auto* p = audioProcessor.apvts.getParameter (id))
        {
            const auto range = p->getNormalisableRange();
            o->setProperty ("id", id);
            o->setProperty ("min", (double) range.start);
            o->setProperty ("max", (double) range.end);
            o->setProperty ("step", range.interval > 0.0f ? (double) range.interval : 0.01);
            o->setProperty ("value", (double) p->convertFrom0to1 (p->getValue()));
            o->setProperty ("label", p->getLabel());
        }

        return juce::var (o);
    };

    juce::Array<juce::var> paramList;
    paramList.add (describeParam (ClipOnizerAudioProcessor::idInput));
    paramList.add (describeParam (ClipOnizerAudioProcessor::idThreshold));
    paramList.add (describeParam (ClipOnizerAudioProcessor::idSoftness));
    paramList.add (describeParam (ClipOnizerAudioProcessor::idOutput));
    paramList.add (describeParam (ClipOnizerAudioProcessor::idSaturator));

    obj->setProperty ("params", paramList);
    obj->setProperty ("delta", audioProcessor.apvts.getRawParameterValue (ClipOnizerAudioProcessor::idDelta)->load() > 0.5f);
    obj->setProperty ("barsOnScreen", barsOnScreen);

    // Сторінка щойно (пере)завантажилась — наступний пуш має бути повним.
    previousDirty = true;
    lastPushedMeta.clear();

    return juce::var (obj);
}

//==============================================================================
// TIME DIVISION — 1:1 порт зі старого OscilloscopeComponent::setTimeDivision
//==============================================================================
void ClipOnizerAudioProcessorEditor::setTimeDivision (float bars) noexcept
{
    barsOnScreen = juce::jlimit (0.25f, 8.0f, bars);

    previousBlockSamples = blockSamples;
    previousBlockId = currentBlockId;
    previousDirty = true;

    blockSamples.clear();
    currentBlockId = std::numeric_limits<int64_t>::min();

    lastCapturedCounter = audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);

    audioProcessor.setOscilloscopeBars (barsOnScreen);
}

//==============================================================================
// CAPTURE — 1:1 порт зі старого OscilloscopeComponent::captureNewSamples
//==============================================================================
void ClipOnizerAudioProcessorEditor::captureNewSamples()
{
    const int64_t blockId =
        audioProcessor.scopeBlockId.load (std::memory_order_acquire);

    const int64_t blockStart =
        audioProcessor.scopeBlockStartCounter.load (std::memory_order_acquire);

    const int64_t writeCounter =
        audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);

    if (blockId == std::numeric_limits<int64_t>::min() || writeCounter <= 0)
        return;

    if (currentBlockId != blockId)
    {
        if (lastCapturedCounter < blockStart)
        {
            const int64_t tail = blockStart - lastCapturedCounter;

            for (int64_t n = 0; n < tail; ++n)
            {
                const int64_t absoluteIndex = lastCapturedCounter + n;
                const int index = static_cast<int> (absoluteIndex % ClipOnizerAudioProcessor::scopeBufferSize);

                blockSamples.push_back (audioProcessor.scopeBuffer[static_cast<size_t> (index)]);
            }

            lastCapturedCounter = blockStart;
        }

        if (currentBlockId != std::numeric_limits<int64_t>::min() && ! blockSamples.empty())
        {
            previousBlockSamples = blockSamples;
            previousBlockId = currentBlockId;
            previousDirty = true;
        }

        currentBlockId = blockId;
        blockSamples.clear();
        lastCapturedCounter = blockStart;
    }

    const int64_t blockIdBeforeCopy = blockId;

    if (lastCapturedCounter < blockStart)
        lastCapturedCounter = blockStart;

    int64_t available = writeCounter - lastCapturedCounter;

    if (available <= 0)
        return;

    if (available > ClipOnizerAudioProcessor::scopeBufferSize)
    {
        lastCapturedCounter = writeCounter - ClipOnizerAudioProcessor::scopeBufferSize;
        available = ClipOnizerAudioProcessor::scopeBufferSize;
        blockSamples.clear();
    }

    const size_t maxStored = static_cast<size_t> (ClipOnizerAudioProcessor::scopeBufferSize);

    for (int64_t n = 0; n < available; ++n)
    {
        const int64_t absoluteIndex = lastCapturedCounter + n;
        const int index = static_cast<int> (absoluteIndex % ClipOnizerAudioProcessor::scopeBufferSize);

        blockSamples.push_back (audioProcessor.scopeBuffer[static_cast<size_t> (index)]);
    }

    lastCapturedCounter = writeCounter;

    const int64_t blockIdAfterCopy =
        audioProcessor.scopeBlockId.load (std::memory_order_acquire);

    if (blockIdAfterCopy != blockIdBeforeCopy)
    {
        if (! blockSamples.empty())
        {
            previousBlockSamples = blockSamples;
            previousBlockId = blockIdBeforeCopy;
            previousDirty = true;
        }

        blockSamples.clear();
        currentBlockId = blockIdAfterCopy;

        lastCapturedCounter =
            audioProcessor.scopeBlockStartCounter.load (std::memory_order_acquire);
    }

    if (blockSamples.size() > maxStored)
        blockSamples.erase (blockSamples.begin(),
                             blockSamples.begin() + static_cast<ptrdiff_t> (blockSamples.size() - maxStored));

    if (previousBlockSamples.size() > maxStored)
        previousBlockSamples.erase (previousBlockSamples.begin(),
                                     previousBlockSamples.begin()
                                         + static_cast<ptrdiff_t> (previousBlockSamples.size() - maxStored));
}

//==============================================================================
// WAVEFORM DOWNSAMPLING -> compact flat JSON array of ints:
//   [min0,max0,min1,max1,...], значення x1000 (JS ділить на 1000).
// Порожній хвіст (ще не записані бакети) не відправляється.
//==============================================================================
void ClipOnizerAudioProcessorEditor::appendWaveformJson (juce::String& json,
                                                         const std::vector<ScopeSample>& samples,
                                                         int buckets,
                                                         double referenceTotalSamples,
                                                         float ScopeSample::* field) const
{
    json << "[";

    if (samples.empty() || buckets <= 0)
    {
        json << "]";
        return;
    }

    const double totalForMapping = juce::jmax (1.0, referenceTotalSamples);
    const size_t availableSamples = samples.size();

    for (int b = 0; b < buckets; ++b)
    {
        const size_t sampleA = static_cast<size_t> ((static_cast<double> (b) / static_cast<double> (buckets)) * totalForMapping);

        if (sampleA >= availableSamples)
            break;

        if (b > 0)
            json << ",";

        size_t sampleB = static_cast<size_t> ((static_cast<double> (b + 1) / static_cast<double> (buckets)) * totalForMapping);
        sampleB = juce::jmax (sampleB, sampleA + static_cast<size_t> (1));
        sampleB = juce::jmin (sampleB, availableSamples);

        float minV = 1.0e6f;
        float maxV = -1.0e6f;

        for (size_t s = sampleA; s < sampleB; ++s)
        {
            const float v = samples[s].*field;
            minV = juce::jmin (minV, v);
            maxV = juce::jmax (maxV, v);
        }

        json << juce::roundToInt (minV * 1000.0f) << "," << juce::roundToInt (maxV * 1000.0f);
    }

    json << "]";
}

//==============================================================================
// PUSH STATE -> JS  (window.__pluginUpdate(state))
//==============================================================================
void ClipOnizerAudioProcessorEditor::pushStateToBrowser()
{
    auto valFor = [this] (const char* id)
    {
        return (double) audioProcessor.apvts.getRawParameterValue (id)->load();
    };

    const double bpm = juce::jmax (20.0, audioProcessor.currentBpm.load (std::memory_order_acquire));
    const double qnPerBar = juce::jmax (0.25, audioProcessor.currentQuarterNotesPerBar.load (std::memory_order_acquire));
    const double sampleRate = audioProcessor.getSampleRate() > 0.0 ? audioProcessor.getSampleRate() : 44100.0;
    const double totalQuarterNotes = qnPerBar * static_cast<double> (barsOnScreen);
    const double expectedBlockSamples = juce::jmax (1.0, (60.0 / bpm) * sampleRate * totalQuarterNotes);

    const float progress = juce::jlimit (0.0f, 1.0f,
        (float) (static_cast<double> (blockSamples.size()) / expectedBlockSamples));

    // ---- "meta" частина: параметри, LED, піки, темп ----
    juce::String meta;
    meta.preallocateBytes (512);
    meta << "\"params\":{"
         <<     "\"input\":"     << juce::String (valFor (ClipOnizerAudioProcessor::idInput), 3)     << ","
         <<     "\"output\":"    << juce::String (valFor (ClipOnizerAudioProcessor::idOutput), 3)    << ","
         <<     "\"threshold\":" << juce::String (valFor (ClipOnizerAudioProcessor::idThreshold), 3) << ","
         <<     "\"softness\":"  << juce::String (valFor (ClipOnizerAudioProcessor::idSoftness), 3)  << ","
         <<     "\"saturator\":" << juce::String (valFor (ClipOnizerAudioProcessor::idSaturator), 3) << ","
         <<     "\"delta\":"     << (valFor (ClipOnizerAudioProcessor::idDelta) > 0.5 ? "true" : "false")
         << "},"
         << "\"clipLevel\":"    << juce::String (audioProcessor.clipIndicatorLevel.load (std::memory_order_acquire), 4) << ","
         << "\"livePeakPos\":"  << juce::String (audioProcessor.scopeLivePeakPos.load (std::memory_order_acquire), 4)   << ","
         << "\"livePeakNeg\":"  << juce::String (audioProcessor.scopeLivePeakNeg.load (std::memory_order_acquire), 4)   << ","
         << "\"bpm\":"          << juce::String (bpm, 2) << ","
         << "\"qnPerBar\":"     << juce::String (qnPerBar, 3) << ","
         << "\"barsOnScreen\":" << juce::String (barsOnScreen, 3);

    // ---- Пропуск порожніх тіків: нових семплів немає і нічого не змінилось ----
    const int64_t writeCounter = audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);

    if (writeCounter == lastPushedWriteCounter && ! previousDirty && meta == lastPushedMeta)
        return;

    lastPushedWriteCounter = writeCounter;
    lastPushedMeta = meta;

    // ---- Повний пакет ----
    juce::String json;
    json.preallocateBytes (static_cast<size_t> (meta.length()) + (previousDirty ? 24000u : 12000u) + 160u);

    json << "if(window.__pluginUpdate)window.__pluginUpdate({" << meta << ","
         << "\"waveform\":{"
         <<     "\"n\":"        << waveformBuckets << ","
         <<     "\"progress\":" << juce::String (progress, 4) << ","
         <<     "\"current\":";
    appendWaveformJson (json, blockSamples, waveformBuckets, expectedBlockSamples);
    json << ",\"outCurrent\":";
    appendWaveformJson (json, blockSamples, waveformBuckets, expectedBlockSamples, &ScopeSample::out);

    if (previousDirty)
    {
        ++previousSerial;
        json << ",\"prevId\":" << previousSerial << ",\"previous\":";
        appendWaveformJson (json, previousBlockSamples, waveformBuckets, expectedBlockSamples);
        json << ",\"outPrevious\":";
        appendWaveformJson (json, previousBlockSamples, waveformBuckets, expectedBlockSamples, &ScopeSample::out);
        previousDirty = false;
    }

    json << "}});";

    webView.evaluateJavascript (json);
}

//==============================================================================
void ClipOnizerAudioProcessorEditor::timerCallback()
{
    captureNewSamples();
    pushStateToBrowser();
}
