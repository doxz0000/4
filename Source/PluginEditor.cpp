/*
    MODEL-CLIPONIZER — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.cpp
*/

#include "PluginEditor.h"
#include "BinaryData.h"
#include "WebViewWarmup.h"
#include <cmath>

namespace
{
    juce::WebBrowserComponent::Resource makeResource (const void* data, int size, const juce::String& mime)
    {
        auto* bytePtr = static_cast<const std::byte*> (data);
        return { std::vector<std::byte> (bytePtr, bytePtr + size), mime };
    }

    // Колір, яким WebView2 заливає себе до першого кадру сторінки (замість
    // білого за замовчуванням). Це середній колір background.png — тож
    // перехід "нативний фон -> WebView" майже непомітний.
    // Має збігатися з --bootBg у style.css.
    const juce::Colour webViewBootColour { 0xff111610 };

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
                .withUserDataFolder (cliponizer::getWebViewDataFolder())
                .withBackgroundColour (webViewBootColour)
                .withStatusBarDisabled()
                .withBuiltInErrorPageDisabled())
            // Початковий стан параметрів вшивається в сторінку ще до запуску
            // скриптів — JS не чекає окремого round-trip getInitialState.
            .withInitialisationData ("initialState", editor.jsGetInitialState ({}))
            .withNativeFunction ("uiReady",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsUiReady (args)); })
            .withNativeFunction ("uiShown",
                [&editor] (const juce::Array<juce::var>& args, juce::WebBrowserComponent::NativeFunctionCompletion completion)
                { completion (editor.jsUiShown (args)); })
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
    // Декодуємо фон один раз (ImageCache тримає його між відкриттями вікна).
    backgroundImage = juce::ImageCache::getFromMemory (BinaryData::background_png,
                                                       BinaryData::background_pngSize);

    // WebView спершу прихований: поки WebView2 стартує і вантажить сторінку,
    // видно нативний фон з paint(). Інакше JUCE малює на місці ще не
    // створеного WebView2 білий прямокутник (WebBrowserComponent::paint ->
    // fallbackPaint). Контролер WebView2 створюється і для прихованого
    // компонента, щойно в редактора з'являється вікно (peer).
    // Показуємо його, коли JS скаже "uiShown" (картинки готові).
    addChildComponent (webView);

    // Страховка: якщо JS з якоїсь причини не відповів — показати все одно.
    juce::Timer::callAfterDelay (3000, [safeThis = juce::Component::SafePointer<ClipOnizerAudioProcessorEditor> (this)]
    {
        if (safeThis != nullptr)
            safeThis->showWebView();
    });

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

// Видно лише до моменту, поки WebView прихований (сторінка ще вантажиться).
// Повторює фон #app зі style.css (background.png + затемнювальний градієнт
// + рамка), щоб підміна на WebView була непомітною.
void ClipOnizerAudioProcessorEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    g.fillAll (webViewBootColour);

    if (backgroundImage.isValid())
        g.drawImage (backgroundImage, bounds, juce::RectanglePlacement::fillDestination);

    // linear-gradient(180deg, rgb(61 59 55 / 21%), rgb(43 42 40 / 50%))
    g.setGradientFill (juce::ColourGradient (juce::Colour (61, 59, 55).withAlpha (0.21f), 0.0f, 0.0f,
                                             juce::Colour (43, 42, 40).withAlpha (0.50f), 0.0f, bounds.getBottom(),
                                             false));
    g.fillRect (bounds);

    // border: 2px solid var(--metalEdge)
    g.setColour (juce::Colour (0xff5a564e));
    g.drawRect (getLocalBounds(), 2);
}

void ClipOnizerAudioProcessorEditor::showWebView()
{
    if (! webView.isVisible())
        webView.setVisible (true);
}

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
    dirtyFrom = 0;
    lastSentFilled = 0;
    lastPushedMeta.clear();

    return juce::var (obj);
}

juce::var ClipOnizerAudioProcessorEditor::jsUiShown (const juce::Array<juce::var>&)
{
    // Сторінка намальована і картинки декодовані — показуємо WebView.
    showWebView();
    return true;
}

juce::var ClipOnizerAudioProcessorEditor::jsUiReady (const juce::Array<juce::var>&)
{
    // Сторінка (пере)ініціалізувалась — наступний пуш має бути повним.
    uiReady = true;
    previousDirty = true;
    dirtyFrom = 0;
    lastSentFilled = 0;
    lastPushedMeta.clear();
    return true;
}

//==============================================================================
// DISPLAY WAVE — 600 колонок min/max одного такту
//==============================================================================
void ClipOnizerAudioProcessorEditor::DisplayWave::clear() noexcept
{
    filled = 0;
    samples = 0;
}

void ClipOnizerAudioProcessorEditor::DisplayWave::addBucket (int column, const ScopeBucket& b) noexcept
{
    if (column < 0 || column >= waveformBuckets)
        return;

    if (column < filled)
    {
        minV[(size_t) column]   = juce::jmin (minV[(size_t) column],   b.minV);
        maxV[(size_t) column]   = juce::jmax (maxV[(size_t) column],   b.maxV);
        minOut[(size_t) column] = juce::jmin (minOut[(size_t) column], b.minOut);
        maxOut[(size_t) column] = juce::jmax (maxOut[(size_t) column], b.maxOut);
        return;
    }

    // Нова колонка (і, про всяк випадок, будь-який пропуск перед нею) —
    // заповнюємо, щоб у хвилі не було дірок.
    for (int c = filled; c <= column; ++c)
    {
        minV[(size_t) c]   = b.minV;
        maxV[(size_t) c]   = b.maxV;
        minOut[(size_t) c] = b.minOut;
        maxOut[(size_t) c] = b.maxOut;
    }

    filled = column + 1;
}

double ClipOnizerAudioProcessorEditor::expectedBlockSamples (float bars) const noexcept
{
    const double bpm = juce::jmax (20.0, audioProcessor.currentBpm.load (std::memory_order_acquire));
    const double qnPerBar = juce::jmax (0.25, audioProcessor.currentQuarterNotesPerBar.load (std::memory_order_acquire));
    const double sampleRate = audioProcessor.getSampleRate() > 0.0 ? audioProcessor.getSampleRate() : 44100.0;
    return juce::jmax (1.0, (60.0 / bpm) * sampleRate * qnPerBar * static_cast<double> (bars));
}

// Перераховує колонки під іншу довжину такту (ratio = oldExpected / newExpected),
// так само як старий код перемаповував сирі семпли previous на нову ширину.
void ClipOnizerAudioProcessorEditor::resampleWave (DisplayWave& wave, double ratio)
{
    if (wave.filled == 0 || ratio <= 0.0 || std::abs (ratio - 1.0) < 1.0e-9)
        return;

    DisplayWave out;
    out.clear();

    for (int c = 0; c < waveformBuckets; ++c)
    {
        const int s0 = static_cast<int> (static_cast<double> (c) / ratio);

        if (s0 >= wave.filled)
            break;

        int s1 = static_cast<int> (std::ceil (static_cast<double> (c + 1) / ratio)) - 1;
        s1 = juce::jlimit (s0, wave.filled - 1, s1);

        ScopeBucket m { wave.minV[(size_t) s0], wave.maxV[(size_t) s0],
                        wave.minOut[(size_t) s0], wave.maxOut[(size_t) s0] };

        for (int s = s0 + 1; s <= s1; ++s)
        {
            m.minV   = juce::jmin (m.minV,   wave.minV[(size_t) s]);
            m.maxV   = juce::jmax (m.maxV,   wave.maxV[(size_t) s]);
            m.minOut = juce::jmin (m.minOut, wave.minOut[(size_t) s]);
            m.maxOut = juce::jmax (m.maxOut, wave.maxOut[(size_t) s]);
        }

        out.addBucket (c, m);
    }

    out.samples = wave.samples;
    wave = out;
}

//==============================================================================
// TIME DIVISION
//==============================================================================
void ClipOnizerAudioProcessorEditor::setTimeDivision (float bars) noexcept
{
    const double oldExpected = expectedBlockSamples (barsOnScreen);
    barsOnScreen = juce::jlimit (0.25f, 8.0f, bars);
    const double newExpected = expectedBlockSamples (barsOnScreen);

    previous = current;
    resampleWave (previous, oldExpected / newExpected);
    previousDirty = true;

    current.clear();
    currentBlockId = std::numeric_limits<int64_t>::min();
    dirtyFrom = 0;
    lastSentFilled = 0;

    lastCapturedCounter = audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);

    audioProcessor.setOscilloscopeBars (barsOnScreen);
}

//==============================================================================
// CAPTURE — читає лише НОВІ бакети процесора і розкладає їх по 600 колонках.
// Вартість O(нових даних), а не O(всього такту), як було з сирими семплами.
//==============================================================================
void ClipOnizerAudioProcessorEditor::startNewCurrentBlock (int64_t blockId, int64_t blockStart)
{
    currentBlockId = blockId;
    currentBlockStart = blockStart;
    current.clear();
    lastCapturedCounter = blockStart;
    dirtyFrom = 0;
    lastSentFilled = 0;
}

void ClipOnizerAudioProcessorEditor::addBuckets (int64_t from, int64_t to,
                                                 int64_t blockStart, double expected)
{
    constexpr int64_t bucketSamples = ClipOnizerAudioProcessor::scopeBucketSamples;
    constexpr int64_t ringSize = ClipOnizerAudioProcessor::scopeBufferSize;

    const double columnsPerSample = static_cast<double> (waveformBuckets) / juce::jmax (1.0, expected);

    from = juce::jmax (from, blockStart);

    for (int64_t abs = from; abs < to; ++abs)
    {
        const int64_t s0 = (abs - blockStart) * bucketSamples;
        const int c0 = static_cast<int> (static_cast<double> (s0) * columnsPerSample);

        if (c0 >= waveformBuckets)
            break;

        const int c1 = juce::jmin (waveformBuckets - 1,
            static_cast<int> (static_cast<double> (s0 + bucketSamples - 1) * columnsPerSample));

        const auto& bucket = audioProcessor.scopeBuffer[static_cast<size_t> (abs % ringSize)];

        for (int c = c0; c <= c1; ++c)
            current.addBucket (c, bucket);

        dirtyFrom = juce::jmin (dirtyFrom, c0);
    }

    if (to > blockStart)
        current.samples = juce::jmax (current.samples, (to - blockStart) * bucketSamples);
}

void ClipOnizerAudioProcessorEditor::captureNewSamples()
{
    constexpr int64_t ringSize = ClipOnizerAudioProcessor::scopeBufferSize;
    constexpr int64_t noBlock = std::numeric_limits<int64_t>::min();

    // Консистентний знімок: процесор пише start, потім id (release), а бакети
    // нового блоку — вже після цього. Якщо id не змінився між першим і
    // останнім читанням, то start/counter належать саме цьому блоку.
    int64_t blockId = noBlock, blockStart = 0, writeCounter = 0;

    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const int64_t id1 = audioProcessor.scopeBlockId.load (std::memory_order_acquire);
        blockStart   = audioProcessor.scopeBlockStartCounter.load (std::memory_order_acquire);
        writeCounter = audioProcessor.scopeWriteCounter.load (std::memory_order_acquire);
        blockId      = audioProcessor.scopeBlockId.load (std::memory_order_acquire);

        if (id1 == blockId)
            break;
    }

    if (blockId == noBlock || writeCounter <= 0)
        return;

    // Процесор скинув лічильники (prepareToPlay) — починаємо з чистого такту.
    if (lastCapturedCounter > writeCounter || currentBlockStart > writeCounter)
    {
        currentBlockId = noBlock;
        lastCapturedCounter = blockStart;
    }

    const double expected = expectedBlockSamples (barsOnScreen);

    if (currentBlockId != blockId)
    {
        if (currentBlockId != noBlock)
        {
            // Дочитуємо хвіст старого такту.
            const int64_t tailFrom = juce::jmax (lastCapturedCounter, writeCounter - ringSize);

            if (tailFrom < blockStart)
                addBuckets (tailFrom, blockStart, currentBlockStart, expected);

            if (current.filled > 0)
            {
                previous = current;
                previousDirty = true;
            }
        }

        startNewCurrentBlock (blockId, blockStart);
    }

    const int64_t from = juce::jmax (juce::jmax (lastCapturedCounter, blockStart),
                                     writeCounter - ringSize);

    if (writeCounter > from)
        addBuckets (from, writeCounter, currentBlockStart, expected);

    lastCapturedCounter = writeCounter;
}

//==============================================================================
// WAVEFORM -> compact flat JSON array of ints:
//   [min0,max0,min1,max1,...] для колонок [from, to), значення x1000.
//==============================================================================
void ClipOnizerAudioProcessorEditor::appendWaveformJson (juce::String& json, const DisplayWave& wave,
                                                         bool outField, int from, int to)
{
    json << "[";

    to = juce::jmin (to, wave.filled);

    const auto& mins = outField ? wave.minOut : wave.minV;
    const auto& maxs = outField ? wave.maxOut : wave.maxV;

    for (int c = juce::jmax (0, from); c < to; ++c)
    {
        if (c > from)
            json << ",";

        json << juce::roundToInt (mins[(size_t) c] * 1000.0f) << ","
             << juce::roundToInt (maxs[(size_t) c] * 1000.0f);
    }

    json << "]";
}

//==============================================================================
// PUSH STATE -> JS  (window.__pluginUpdate(state))
//==============================================================================
void ClipOnizerAudioProcessorEditor::pushStateToBrowser()
{
    if (! uiReady)
        return;

    auto valFor = [this] (const char* id)
    {
        return (double) audioProcessor.apvts.getRawParameterValue (id)->load();
    };

    const double bpm = juce::jmax (20.0, audioProcessor.currentBpm.load (std::memory_order_acquire));
    const double qnPerBar = juce::jmax (0.25, audioProcessor.currentQuarterNotesPerBar.load (std::memory_order_acquire));
    const double expected = expectedBlockSamples (barsOnScreen);

    const float progress = juce::jlimit (0.0f, 1.0f,
        (float) (static_cast<double> (current.samples) / expected));

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

    // ---- Пропуск порожніх тіків: нових даних немає і нічого не змінилось ----
    const bool currentDirty = dirtyFrom < waveformBuckets;

    if (! currentDirty && ! previousDirty && meta == lastPushedMeta)
        return;

    lastPushedMeta = meta;

    // ---- Пакет: current/outCurrent — лише змінені колонки [from, filled) ----
    const int to = current.filled;
    const int from = juce::jlimit (0, to, juce::jmin (dirtyFrom, lastSentFilled));

    juce::String json;
    json.preallocateBytes (static_cast<size_t> (meta.length())
                           + static_cast<size_t> (to - from) * 24u
                           + (previousDirty ? 24000u : 0u) + 200u);

    json << "if(window.__pluginUpdate)window.__pluginUpdate({" << meta << ","
         << "\"waveform\":{"
         <<     "\"n\":"        << waveformBuckets << ","
         <<     "\"progress\":" << juce::String (progress, 4) << ","
         <<     "\"from\":"     << from << ","
         <<     "\"current\":";
    appendWaveformJson (json, current, false, from, to);
    json << ",\"outCurrent\":";
    appendWaveformJson (json, current, true, from, to);

    if (previousDirty)
    {
        ++previousSerial;
        json << ",\"prevId\":" << previousSerial << ",\"previous\":";
        appendWaveformJson (json, previous, false, 0, previous.filled);
        json << ",\"outPrevious\":";
        appendWaveformJson (json, previous, true, 0, previous.filled);
        previousDirty = false;
    }

    json << "}});";

    dirtyFrom = waveformBuckets;
    lastSentFilled = to;

    webView.evaluateJavascript (json);
}

//==============================================================================
void ClipOnizerAudioProcessorEditor::timerCallback()
{
    // Захоплення дешеве і йде завжди (щоб не втратити дані такту);
    // пуш у WebView — лише коли вікно реально видно.
    captureNewSamples();

    if (isShowing())
        pushStateToBrowser();
}
