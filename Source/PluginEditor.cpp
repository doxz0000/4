/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.cpp
*/

#include "PluginEditor.h"

//==============================================================================
// LOOK AND FEEL
//==============================================================================
ClipOnizerLookAndFeel::ClipOnizerLookAndFeel()
{
    setColour (juce::Slider::thumbColourId, ClipOnizerColours::textAmber);
    setColour (juce::Slider::rotarySliderFillColourId, ClipOnizerColours::traceSoftClip);
    setColour (juce::Slider::rotarySliderOutlineColourId, ClipOnizerColours::metalEdge);
    setColour (juce::Slider::textBoxTextColourId, ClipOnizerColours::textAmber);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, ClipOnizerColours::textAmber);
    setColour (juce::TextButton::buttonColourId, ClipOnizerColours::panelLight);
    setColour (juce::TextButton::buttonOnColourId, ClipOnizerColours::traceHardClip);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::TextButton::textColourOffId, ClipOnizerColours::textAmber);
}

void ClipOnizerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                                               juce::Slider&)
{
    // ЗАГЛУШКА ПІД СПРАЙТ: тут малюється проста металева ручка з дугою прогресу
    // і стрілкою. Коли буде картинка дизайну — цей метод стане місцем,
    // де замість fillEllipse/Path буде drawImage повернутого на sliderPos кута спрайту.
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    juce::ColourGradient metalGrad (ClipOnizerColours::panelLight, centre.x, centre.y - radius,
                                     ClipOnizerColours::panelDark, centre.x, centre.y + radius, false);
    g.setGradientFill (metalGrad);
    g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

    g.setColour (ClipOnizerColours::metalEdge);
    g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.5f);

    juce::Path arcTrack;
    arcTrack.addCentredArc (centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (ClipOnizerColours::metalEdge.darker (0.4f));
    g.strokePath (arcTrack, juce::PathStrokeType (2.5f));

    juce::Path arcValue;
    arcValue.addCentredArc (centre.x, centre.y, radius - 3.0f, radius - 3.0f, 0.0f,
                             rotaryStartAngle, angle, true);
    g.setColour (ClipOnizerColours::traceSoftClip);
    g.strokePath (arcValue, juce::PathStrokeType (2.5f));

    juce::Path pointer;
    const float pointerLength = radius * 0.75f;
    pointer.addRectangle (-1.5f, -pointerLength, 3.0f, pointerLength);
    pointer.applyTransform (juce::AffineTransform::rotation (angle).translated (centre));
    g.setColour (ClipOnizerColours::textAmber);
    g.fillPath (pointer);

    g.setColour (ClipOnizerColours::metalEdge);
    g.fillEllipse (centre.x - 2.5f, centre.y - 2.5f, 5.0f, 5.0f);
}

//==============================================================================
// CLIP SHAPER (transfer function)
//==============================================================================
ClipShaperComponent::ClipShaperComponent (ClipOnizerAudioProcessor& p) : processor (p)
{
    startTimerHz (30);
}

void ClipShaperComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (ClipOnizerColours::scopeGlassBg);
    g.fillRoundedRectangle (bounds, 6.0f);

    auto plot = bounds.reduced (26.0f);

    const float thresholdLin = processor.getThresholdLinear();
    const float knee01       = processor.getSoftnessNormalized();

    // Робочий діапазон амплітуди по обох осях: 0 .. 2.0 (лінійно),
    // щоб було видно і Threshold вище 0 dB (0 dB = амплітуда 1.0).
    constexpr float maxAmp = 2.0f;
    // ВИПРАВЛЕННЯ: MSVC вимагає явного захоплення навіть constexpr-змінних
    // у лямбдах у цьому контексті (звідси були C3493/C2064/C2737).
    auto ampToNorm = [maxAmp] (float amp) { return juce::jlimit (0.0f, 1.0f, amp / maxAmp); };

    // --- Сітка dB ---
    g.setColour (ClipOnizerColours::scopeGrid);
    for (float db : { -24.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        const float norm = ampToNorm (juce::Decibels::decibelsToGain (db));
        const float x = plot.getX() + norm * plot.getWidth();
        const float y = plot.getBottom() - norm * plot.getHeight();
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
    }

    // --- Референсна діагональ Input = Output (до clip) ---
    g.setColour (ClipOnizerColours::traceNormal.withAlpha (0.3f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);

    // --- Реальна transfer curve, побудована по кроках і розфарбована по clipAmount ---
    constexpr int steps = 200;
    for (int i = 0; i < steps; ++i)
    {
        const float xAmpA = (i       / (float) steps) * maxAmp;
        const float xAmpB = ((i + 1) / (float) steps) * maxAmp;

        const auto rA = ClipShaper::process (xAmpA, thresholdLin, knee01);
        const auto rB = ClipShaper::process (xAmpB, thresholdLin, knee01);

        const juce::Colour segColour = rA.second < 0.001f ? ClipOnizerColours::traceNormal
                                      : (rA.second < 0.85f ? ClipOnizerColours::traceSoftClip
                                                            : ClipOnizerColours::traceHardClip);
        g.setColour (segColour);

        const float pxA = plot.getX() + ampToNorm (xAmpA) * plot.getWidth();
        const float pxB = plot.getX() + ampToNorm (xAmpB) * plot.getWidth();
        const float pyA = plot.getBottom() - ampToNorm (rA.first) * plot.getHeight();
        const float pyB = plot.getBottom() - ampToNorm (rB.first) * plot.getHeight();

        g.drawLine (pxA, pyA, pxB, pyB, 2.2f);
    }

    // --- THRESHOLD LINE (та ж лінія, що і на осцилографі) + цифрове значення ---
    const float threshY = plot.getBottom() - ampToNorm (thresholdLin) * plot.getHeight();
    g.setColour (ClipOnizerColours::thresholdLine);
    float dash[2] = { 5.0f, 4.0f };
    g.drawDashedLine (juce::Line<float> (plot.getX(), threshY, plot.getRight(), threshY), dash, 2, 1.2f);

    const float thresholdDb = juce::Decibels::gainToDecibels (thresholdLin);
    g.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
    g.drawText ((thresholdDb > 0.0f ? "THRESHOLD +" : "THRESHOLD ") + juce::String (thresholdDb, 1) + " dB",
                (int) plot.getX(), (int) threshY - 16, 200, 14, juce::Justification::left);

    // --- LIVE SIGNAL DOT: реальний рівень сигналу рухається по кривій ---
    const int writePos = processor.scopeWritePos.load (std::memory_order_relaxed);
    const int bufSize  = ClipOnizerAudioProcessor::scopeBufferSize;
    const int lastIdx  = ((writePos - 1) % bufSize + bufSize) % bufSize;
    const float liveAmp = std::abs (processor.scopeBuffer[(size_t) lastIdx].value);

    liveDotSmoothed += (liveAmp - liveDotSmoothed) * 0.25f;

    const auto dotResult = ClipShaper::process (liveDotSmoothed, thresholdLin, knee01);
    const float dotPx = plot.getX() + ampToNorm (liveDotSmoothed) * plot.getWidth();
    const float dotPy = plot.getBottom() - ampToNorm (dotResult.first) * plot.getHeight();

    const juce::Colour dotColour = dotResult.second < 0.001f ? ClipOnizerColours::traceNormal
                                  : (dotResult.second < 0.85f ? ClipOnizerColours::traceSoftClip
                                                               : ClipOnizerColours::traceHardClip);
    g.setColour (dotColour);
    g.fillEllipse (dotPx - 4.5f, dotPy - 4.5f, 9.0f, 9.0f);
    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawEllipse (dotPx - 4.5f, dotPy - 4.5f, 9.0f, 9.0f, 1.0f);

    // --- Підпис аналізатора + рамка ---
    g.setColour (ClipOnizerColours::textAmber.withAlpha (0.8f));
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText ("CLIP SHAPER", bounds.removeFromTop (18).toNearestInt(), juce::Justification::centred);

    g.setColour (ClipOnizerColours::metalEdge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f, 1.5f);
}

//==============================================================================
// BLOCK-BASED REAL-TIME OSCILLOSCOPE
//==============================================================================

OscilloscopeComponent::OscilloscopeComponent (ClipOnizerAudioProcessor& p)
    : processor (p)
{
    blockSamples.reserve (262144);

    lastCapturedWritePos =
        processor.scopeWritePos.load (std::memory_order_relaxed);

    startTimerHz (30);
}

//==============================================================================
// Отримуємо поточну музичну позицію DAW.
//
// Основний режим:
//      PPQ -> точна музична синхронізація.
//
// Fallback:
//      BPM + elapsed time.
//
// Це дозволяє осцилографу працювати і в DAW,
// і в standalone режимі.
//==============================================================================

OscilloscopeComponent::BlockPosition
OscilloscopeComponent::getCurrentBlockPosition()
{
    BlockPosition result;

    const double bpm =
        juce::jmax (20.0, processor.currentBpm.load());

    result.bpm = bpm;

    //--------------------------------------------------------------------------
    // Спочатку пробуємо отримати реальну позицію DAW.
    //--------------------------------------------------------------------------

    if (auto* playHead = processor.getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (auto ppq = position->getPpqPosition())
            {
                // -------------------------------------------------------------
                // Time signature.
                //
                // Якщо DAW її не передала — використовуємо стандартний 4/4.
                // -------------------------------------------------------------

                double quarterNotesPerBar = 4.0;

                if (auto timeSignature = position->getTimeSignature())
                {
                    const int numerator =
                        juce::jmax (1, timeSignature->numerator);

                    const int denominator =
                        juce::jmax (1, timeSignature->denominator);

                    quarterNotesPerBar =
                        static_cast<double> (numerator)
                        * (4.0 / static_cast<double> (denominator));
                }

                const double blockLength =
                    quarterNotesPerBar * static_cast<double> (barsOnScreen);

                // Позиція всередині музичного блоку.
                const double absoluteBlockPosition =
                    *ppq / blockLength;

                const auto blockId =
                    static_cast<int64_t> (
                        std::floor (absoluteBlockPosition));

                double positionInBlock =
                    *ppq
                    - static_cast<double> (blockId) * blockLength;

                // Захист від дуже маленьких floating-point похибок.
                if (positionInBlock < 0.0)
                    positionInBlock = 0.0;

                if (positionInBlock > blockLength)
                    positionInBlock = blockLength;

                result.valid = true;
                result.blockId = blockId;
                result.positionInBlock = positionInBlock;
                result.blockLengthInQuarterNotes = blockLength;

                return result;
            }
        }
    }

    //--------------------------------------------------------------------------
    // FALLBACK
    //
    // Якщо PPQ недоступний, працюємо через BPM.
    //--------------------------------------------------------------------------

    const double nowSeconds =
        juce::Time::getMillisecondCounterHiRes() * 0.001;

    // Перший виклик.
    if (fallbackLastTimeSeconds < 0.0)
    {
        fallbackLastTimeSeconds = nowSeconds;
        fallbackBeatPosition = 0.0;
    }
    else
    {
        const double deltaSeconds =
            juce::jlimit (
                0.0,
                0.25,
                nowSeconds - fallbackLastTimeSeconds);

        fallbackLastTimeSeconds = nowSeconds;

        // BPM -> quarter notes.
        fallbackBeatPosition +=
            deltaSeconds * (bpm / 60.0);
    }

    const double quarterNotesPerBar = 4.0;

    const double blockLength =
        quarterNotesPerBar * static_cast<double> (barsOnScreen);

    const double absoluteBlockPosition =
        fallbackBeatPosition / blockLength;

    const auto blockId =
        static_cast<int64_t> (
            std::floor (absoluteBlockPosition));

    double positionInBlock =
        fallbackBeatPosition
        - static_cast<double> (blockId) * blockLength;

    positionInBlock =
        juce::jlimit (0.0, blockLength, positionInBlock);

    result.valid = true;
    result.blockId = blockId;
    result.positionInBlock = positionInBlock;
    result.blockLengthInQuarterNotes = blockLength;

    return result;
}

//==============================================================================
// Починаємо новий музичний блок.
//==============================================================================

void OscilloscopeComponent::startNewBlock (int64_t blockId)
{
    currentBlockId = blockId;

    blockSamples.clear();

    // Не дозволяємо vector постійно перевиділяти пам'ять
    // під час роботи.
    if (blockSamples.capacity() < 4096)
        blockSamples.reserve (4096);
}

//==============================================================================
// Забираємо нові семпли з processor.scopeBuffer.
//
// На відміну від старого коду, ми більше НЕ малюємо:
//
//     writePos - samplesToShow
//
// Тепер кожен новий аудіосемпл додається в поточний
// музичний блок.
//
// Тому waveform фізично не рухається по екрану.
// Він тільки заповнює фіксоване вікно зліва направо.
//==============================================================================

void OscilloscopeComponent::captureNewSamples()
{
    const auto position = getCurrentBlockPosition();

    if (! position.valid)
        return;

    //--------------------------------------------------------------------------
    // Перевіряємо зміну музичного блоку.
    //--------------------------------------------------------------------------

    if (currentBlockId != position.blockId)
    {
        startNewBlock (position.blockId);

        lastCapturedWritePos =
            processor.scopeWritePos.load (
                std::memory_order_relaxed);

        return;
    }

    //--------------------------------------------------------------------------
    // Поточна позиція ring buffer.
    //--------------------------------------------------------------------------

    const int currentWritePos =
        processor.scopeWritePos.load (
            std::memory_order_relaxed);

    const int bufferSize =
        ClipOnizerAudioProcessor::scopeBufferSize;

    int samplesAvailable =
        currentWritePos - lastCapturedWritePos;

    // Ring-buffer wrap.
    if (samplesAvailable < 0)
        samplesAvailable += bufferSize;

    //--------------------------------------------------------------------------
    // Якщо UI сильно відстав від audio thread,
    // старі дані вже могли бути перезаписані.
    //
    // У такому випадку беремо тільки доступну останню частину.
    //--------------------------------------------------------------------------

    if (samplesAvailable >= bufferSize)
    {
        samplesAvailable = bufferSize - 1;

        lastCapturedWritePos =
            currentWritePos - samplesAvailable;

        if (lastCapturedWritePos < 0)
            lastCapturedWritePos += bufferSize;
    }

    //--------------------------------------------------------------------------
    // Додаємо нові семпли до поточного блоку.
    //--------------------------------------------------------------------------

    for (int i = 0; i < samplesAvailable; ++i)
    {
        const int index =
            (lastCapturedWritePos + i) % bufferSize;

        blockSamples.push_back (
            processor.scopeBuffer[(size_t) index]);
    }

    lastCapturedWritePos = currentWritePos;

    //--------------------------------------------------------------------------
    // Захист від неконтрольованого росту.
    //
    // 8 bars при нормальному BPM буде значно менше цього значення.
    // Якщо host має дуже низький BPM — обмежуємо буфер.
    //--------------------------------------------------------------------------

    constexpr size_t maxStoredSamples = 2'000'000;

    if (blockSamples.size() > maxStoredSamples)
    {
        const size_t removeCount =
            blockSamples.size() - maxStoredSamples;

        blockSamples.erase (
            blockSamples.begin(),
            blockSamples.begin() + static_cast<ptrdiff_t> (removeCount));
    }
}

//==============================================================================
// PAINT
//==============================================================================

void OscilloscopeComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    //--------------------------------------------------------------------------
    // Background.
    //--------------------------------------------------------------------------

    g.setColour (ClipOnizerColours::scopeGlassBg);
    g.fillRoundedRectangle (bounds, 6.0f);

    auto plot = bounds.reduced (26.0f, 22.0f);

    //--------------------------------------------------------------------------
    // BPM / musical information.
    //--------------------------------------------------------------------------

    const double bpm =
        juce::jmax (20.0, processor.currentBpm.load());

    //--------------------------------------------------------------------------
    // Vertical amplitude / zoom.
    //--------------------------------------------------------------------------

    constexpr float baseRange = 2.0f;

    const float effectiveRange =
        juce::jmax (
            0.05f,
            baseRange / juce::jmax (0.1f, verticalZoom));

    auto ampToY = [&] (float amp)
    {
        const float norm =
            juce::jlimit (
                -1.0f,
                1.0f,
                amp / effectiveRange);

        return plot.getCentreY()
               - norm * (plot.getHeight() * 0.5f);
    };

    //--------------------------------------------------------------------------
    // Horizontal dB grid.
    //--------------------------------------------------------------------------

    for (float db : { -24.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        const float amp =
            juce::Decibels::decibelsToGain (db);

        const float y = ampToY (amp);

        if (y >= plot.getY() && y <= plot.getBottom())
        {
            g.setColour (ClipOnizerColours::scopeGrid);
            g.drawHorizontalLine (
                static_cast<int> (y),
                plot.getX(),
                plot.getRight());

            g.setColour (
                ClipOnizerColours::textAmber.withAlpha (0.55f));

            g.setFont (10.0f);

            g.drawText (
                (db > 0.0f ? "+" : juce::String())
                    + juce::String (db, 0)
                    + " dB",
                static_cast<int> (plot.getX()) + 3,
                static_cast<int> (y) - 12,
                55,
                12,
                juce::Justification::left);
        }
    }

    //--------------------------------------------------------------------------
    // Музична сітка.
    //
    // Для 4/4:
    //
    // 1 BAR  -> 5 вертикальних ліній
    // 2 BAR  -> 9
    // 4 BAR  -> 17
    // 8 BAR  -> 33
    //
    // Лінії кожної чверті.
    //--------------------------------------------------------------------------

    double quarterNotesPerBar = 4.0;

    if (auto* playHead = processor.getPlayHead())
    {
        if (auto position = playHead->getPosition())
        {
            if (auto timeSignature = position->getTimeSignature())
            {
                const int numerator =
                    juce::jmax (1, timeSignature->numerator);

                const int denominator =
                    juce::jmax (1, timeSignature->denominator);

                quarterNotesPerBar =
                    static_cast<double> (numerator)
                    * (4.0 / static_cast<double> (denominator));
            }
        }
    }

    const double totalQuarterNotes =
        quarterNotesPerBar
        * static_cast<double> (barsOnScreen);

    const int quarterLineCount =
        juce::jmax (
            1,
            static_cast<int> (
                std::ceil (totalQuarterNotes)));

    for (int q = 0; q <= quarterLineCount; ++q)
    {
        const float x =
            plot.getX()
            + (static_cast<float> (q)
               / static_cast<float> (quarterLineCount))
                * plot.getWidth();

        // Трохи яскравіші лінії початку такту.
        const bool isBarLine =
            std::fmod (
                static_cast<double> (q),
                quarterNotesPerBar) < 0.001;

        g.setColour (
            isBarLine
                ? ClipOnizerColours::textAmber.withAlpha (0.28f)
                : ClipOnizerColours::scopeGrid);

        g.drawVerticalLine (
            static_cast<int> (x),
            plot.getY(),
            plot.getBottom());
    }

    //--------------------------------------------------------------------------
    // WAVEFORM
    //
    // ГОЛОВНА ЗМІНА:
    //
    // Немає більше:
    //
    //     startIdx = writePos - samplesToShow
    //
    // Waveform береться з blockSamples.
    //
    // Тому:
    //
    // 1 BAR:
    //
    //     |================|
    //     >>>>>>>>>>>>>>>>
    //
    // потім:
    //
    //     |================|
    //     >>>>>>>>>>>>>>>>
    //
    // Новий блок починається з X = 0.
    // Ніякого постійного scrolling.
    //--------------------------------------------------------------------------

    const int widthPx =
        static_cast<int> (plot.getWidth());

    if (widthPx > 0 && ! blockSamples.empty())
    {
        const size_t totalSamples =
            blockSamples.size();

        //--------------------------------------------------------------------------
        // Скільки семплів приблизно повинно бути в повному блоці.
        //
        // Це використовується ТІЛЬКИ для позиціонування waveform
        // по ширині.
        //--------------------------------------------------------------------------

        const double sampleRate =
            processor.getSampleRate() > 0.0
                ? processor.getSampleRate()
                : 44100.0;

        const double samplesPerQuarter =
            (60.0 / bpm) * sampleRate;

        const double expectedBlockSamples =
            juce::jmax (
                1.0,
                samplesPerQuarter * totalQuarterNotes);

        //--------------------------------------------------------------------------
        // Малюємо waveform тільки в межах вже записаної частини блоку.
        //
        // Тобто waveform НЕ буде розтягнута на весь екран,
        // поки блок ще не дограв.
        //--------------------------------------------------------------------------

        const float progress =
            juce::jlimit (
                0.0f,
                1.0f,
                static_cast<float> (
                    static_cast<double> (totalSamples)
                    / expectedBlockSamples));

        const int visibleWidth =
            juce::jmax (
                1,
                static_cast<int> (
                    progress * static_cast<float> (widthPx)));

        for (int px = 0;
             px < visibleWidth;
             ++px)
        {
            const size_t sampleA =
                static_cast<size_t> (
                    (static_cast<double> (px)
                     / static_cast<double> (visibleWidth))
                    * static_cast<double> (totalSamples));

            size_t sampleB =
                static_cast<size_t> (
                    (static_cast<double> (px + 1)
                     / static_cast<double> (visibleWidth))
                    * static_cast<double> (totalSamples));

            sampleB =
                juce::jmax (
                    sampleB,
                    sampleA + static_cast<size_t> (1));

            sampleB =
                juce::jmin (
                    sampleB,
                    totalSamples);

            if (sampleA >= totalSamples)
                continue;

            float minV = 1.0e6f;
            float maxV = -1.0e6f;
            float maxClip = 0.0f;

            for (size_t s = sampleA;
                 s < sampleB;
                 ++s)
            {
                const auto& sample =
                    blockSamples[s];

                minV =
                    juce::jmin (
                        minV,
                        sample.value);

                maxV =
                    juce::jmax (
                        maxV,
                        sample.value);

                maxClip =
                    juce::jmax (
                        maxClip,
                        sample.clipAmount);
            }

            const juce::Colour waveColour =
                maxClip < 0.001f
                    ? ClipOnizerColours::traceNormal
                    : (maxClip < 0.85f
                        ? ClipOnizerColours::traceSoftClip
                        : ClipOnizerColours::traceHardClip);

            g.setColour (waveColour);

            const float x =
                plot.getX()
                + (static_cast<float> (px)
                   / static_cast<float> (widthPx))
                    * plot.getWidth();

            const float yTop =
                ampToY (maxV);

            const float yBottom =
                ampToY (minV);

            g.drawLine (
                x,
                yTop,
                x,
                juce::jmax (
                    yBottom,
                    yTop + 1.0f),
                1.0f);
        }
    }

    //--------------------------------------------------------------------------
    // THRESHOLD LINE.
    //--------------------------------------------------------------------------

    const float thresholdLin =
        processor.getThresholdLinear();

    const float thresholdY =
        ampToY (thresholdLin);

    g.setColour (
        ClipOnizerColours::thresholdLine);

    float dash[2] =
    {
        5.0f,
        4.0f
    };

    g.drawDashedLine (
        juce::Line<float> (
            plot.getX(),
            thresholdY,
            plot.getRight(),
            thresholdY),
        dash,
        2,
        1.5f);

    const float thresholdDb =
        juce::Decibels::gainToDecibels (
            thresholdLin);

    g.setFont (
        juce::Font (
            juce::Font::getDefaultMonospacedFontName(),
            12.0f,
            juce::Font::bold));

    g.drawText (
        (thresholdDb > 0.0f
            ? "THRESHOLD +"
            : "THRESHOLD ")
            + juce::String (thresholdDb, 1)
            + " dB",
        static_cast<int> (plot.getX()),
        static_cast<int> (thresholdY) - 16,
        200,
        14,
        juce::Justification::left);

    //--------------------------------------------------------------------------
    // Показуємо поточний прогрес блоку.
    //
    // Наприклад:
    //
    // BAR 3 / 8
    //
    // або просто:
    //
    // 1 BAR
    // BPM: 128
    //
    //--------------------------------------------------------------------------

    const auto position =
        getCurrentBlockPosition();

    g.setColour (
        ClipOnizerColours::textAmber.withAlpha (0.65f));

    g.setFont (
        juce::Font (
            juce::Font::getDefaultMonospacedFontName(),
            10.0f,
            juce::Font::bold));

    const int percent =
        ! blockSamples.empty()
            ? static_cast<int> (
                juce::jlimit (
                    0.0,
                    100.0,
                    (static_cast<double> (blockSamples.size())
                     / juce::jmax (
                         1.0,
                         (60.0 / bpm)
                         * processor.getSampleRate()
                         * totalQuarterNotes))
                    * 100.0))
            : 0;

    g.drawText (
        juce::String (barsOnScreen, 2) + " BAR"
            + "   " + juce::String (percent) + "%",
        static_cast<int> (plot.getRight()) - 120,
        static_cast<int> (plot.getY()) - 17,
        120,
        14,
        juce::Justification::right);

    //--------------------------------------------------------------------------
    // Header.
    //--------------------------------------------------------------------------

    g.setColour (
        ClipOnizerColours::textAmber.withAlpha (0.8f));

    g.setFont (
        juce::Font (
            12.0f,
            juce::Font::bold));

    g.drawText (
        "REAL-TIME OSCILLOSCOPE",
        bounds.removeFromTop (18).toNearestInt(),
        juce::Justification::centred);

    //--------------------------------------------------------------------------
    // Frame.
    //--------------------------------------------------------------------------

    g.setColour (
        ClipOnizerColours::metalEdge);

    g.drawRoundedRectangle (
        getLocalBounds().toFloat().reduced (1.0f),
        6.0f,
        1.5f);
}

//==============================================================================
// CLIP INDICATOR (аналогова лампа)
//==============================================================================
ClipIndicatorComponent::ClipIndicatorComponent (ClipOnizerAudioProcessor& p) : processor (p)
{
    startTimerHz (30);
}

void ClipIndicatorComponent::paint (juce::Graphics& g)
{
    const float target = processor.clipIndicatorLevel.load();
    smoothedLevel += (target - smoothedLevel) * 0.2f;

    auto bounds = getLocalBounds().toFloat().reduced (6.0f);
    auto ledArea = bounds.withHeight (bounds.getHeight() - 14.0f);
    const auto centre = ledArea.getCentre();
    const float radius = juce::jmin (ledArea.getWidth(), ledArea.getHeight()) * 0.5f;

    juce::Colour ledColour;
    if (smoothedLevel < 0.05f)
        ledColour = juce::Colours::green.darker (0.3f);
    else if (smoothedLevel < 0.3f)
        ledColour = juce::Colour::fromHSV (juce::jmap (smoothedLevel, 0.05f, 0.3f, 0.33f, 0.16f), 0.9f, 1.0f, 1.0f);
    else if (smoothedLevel < 0.65f)
        ledColour = juce::Colour::fromHSV (juce::jmap (smoothedLevel, 0.3f, 0.65f, 0.16f, 0.02f), 0.95f, 1.0f, 1.0f);
    else
        ledColour = juce::Colours::red.brighter (juce::jmin (0.4f, (smoothedLevel - 0.65f)));

    // Glow (світіння лампи)
    juce::ColourGradient glow (ledColour.withAlpha (0.85f), centre.x, centre.y,
                                ledColour.withAlpha (0.0f), centre.x, centre.y - radius * 2.4f, false);
    g.setGradientFill (glow);
    g.fillEllipse (centre.x - radius * 1.9f, centre.y - radius * 1.9f, radius * 3.8f, radius * 3.8f);

    g.setColour (ClipOnizerColours::ledOff);
    g.fillEllipse (ledArea);

    g.setColour (ledColour);
    g.fillEllipse (ledArea.reduced (radius * 0.15f));

    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawEllipse (ledArea, 1.5f);

    g.setColour (ClipOnizerColours::textAmber);
    g.setFont (juce::Font (10.0f, juce::Font::bold));
    g.drawText ("CLIP", bounds.removeFromBottom (14.0f).toNearestInt(), juce::Justification::centred);
}

//==============================================================================
// EDITOR
//==============================================================================
ClipOnizerAudioProcessorEditor::ClipOnizerAudioProcessorEditor (ClipOnizerAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p),
      clipShaper (p), oscilloscope (p), clipIndicator (p)
{
    setLookAndFeel (&lookAndFeel);

    auto setupKnob = [this] (juce::Slider& s, juce::Label& l, const juce::String& text)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
        s.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                juce::MathConstants<float>::pi * 2.8f, true);
        s.setDoubleClickReturnValue (true, 0.0); // double-click reset у 0 (0 dB / 20% і т.п.)
        addAndMakeVisible (s);

        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (13.0f, juce::Font::bold));
        addAndMakeVisible (l);
    };

    setupKnob (inputSlider,     inputLabel,     "INPUT");
    setupKnob (thresholdSlider, thresholdLabel, "THRESHOLD");
    setupKnob (softnessSlider,  softnessLabel,  "SOFTNESS / KNEE");
    setupKnob (outputSlider,    outputLabel,    "OUTPUT");

    inputAttachment     = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                              (audioProcessor.apvts, ClipOnizerAudioProcessor::idInput, inputSlider);
    thresholdAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                              (audioProcessor.apvts, ClipOnizerAudioProcessor::idThreshold, thresholdSlider);
    softnessAttachment  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                              (audioProcessor.apvts, ClipOnizerAudioProcessor::idSoftness, softnessSlider);
    outputAttachment    = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>
                              (audioProcessor.apvts, ClipOnizerAudioProcessor::idOutput, outputSlider);

    deltaButton.setClickingTogglesState (true);
    addAndMakeVisible (deltaButton);
    deltaAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>
                          (audioProcessor.apvts, ClipOnizerAudioProcessor::idDelta, deltaButton);

    addAndMakeVisible (clipShaper);
    addAndMakeVisible (oscilloscope);
    addAndMakeVisible (clipIndicator);

    const char* labels[6] =
{
    "1/4 BAR",
    "1/2 BAR",
    "1 BAR",
    "2 BAR",
    "4 BAR",
    "8 BAR"
};
    for (int i = 0; i < 6; ++i)
    {
        timeScaleButtons[i].setButtonText (labels[i]);
        timeScaleButtons[i].setClickingTogglesState (true);
        timeScaleButtons[i].setRadioGroupId (1001);
        timeScaleButtons[i].onClick = [this, i] { timeScaleButtonClicked (i); };
        addAndMakeVisible (timeScaleButtons[i]);
    }
    timeScaleButtons[2].setToggleState (true, juce::sendNotification); // дефолт "1"

    bpmLabel.setJustificationType (juce::Justification::centredRight);
    bpmLabel.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold));
    addAndMakeVisible (bpmLabel);

    verticalZoomSlider.setSliderStyle (juce::Slider::LinearVertical);
    verticalZoomSlider.setRange (0.2, 4.0, 0.01);
    verticalZoomSlider.setValue (1.0);
    verticalZoomSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    verticalZoomSlider.onValueChange = [this]
    {
        oscilloscope.setVerticalZoom ((float) verticalZoomSlider.getValue());
    };
    addAndMakeVisible (verticalZoomSlider);

    modelLabel.setText ("MODEL-CLIPONIZER No.8", juce::dontSendNotification);
    modelLabel.setFont (juce::Font (17.0f, juce::Font::bold));
    modelLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (modelLabel);

    subtitleLabel.setText ("DR.DOC SOUNDLAB-EQUIPMENT", juce::dontSendNotification);
    subtitleLabel.setFont (juce::Font (11.0f));
    subtitleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (subtitleLabel);

    titleLabel.setText ("CLIP-TO-ZERO   /   NO LATENCY CLIPPER", juce::dontSendNotification);
    titleLabel.setFont (juce::Font (13.0f, juce::Font::italic));
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    setResizable (true, true);
    setResizeLimits (900, 560, 1800, 1100);
    setSize (1180, 720);

    startTimerHz (10);
}

ClipOnizerAudioProcessorEditor::~ClipOnizerAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void ClipOnizerAudioProcessorEditor::timerCallback()
{
    bpmLabel.setText ("BPM: " + juce::String ((int) std::round (audioProcessor.currentBpm.load())),
                       juce::dontSendNotification);
}

void ClipOnizerAudioProcessorEditor::timeScaleButtonClicked (int index)
{
    oscilloscope.setTimeDivision (timeScaleValues[index]);
}

//==============================================================================
void ClipOnizerAudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (ClipOnizerColours::panelLight, 0.0f, 0.0f,
                              ClipOnizerColours::panelDark, 0.0f, (float) getHeight(), false);
    g.setGradientFill (bg);
    g.fillAll();

    g.setColour (ClipOnizerColours::metalEdge);
    g.drawRect (getLocalBounds(), 2);

    // Декоративні "гвинти" по кутах — заглушка під майбутній спрайт корпусу.
    for (auto corner : { juce::Point<int> (14, 14), juce::Point<int> (getWidth() - 14, 14),
                          juce::Point<int> (14, getHeight() - 14), juce::Point<int> (getWidth() - 14, getHeight() - 14) })
    {
        g.setColour (ClipOnizerColours::metalEdge.darker());
        g.fillEllipse ((float) corner.x - 5.0f, (float) corner.y - 5.0f, 10.0f, 10.0f);
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.drawLine ((float) corner.x - 3.0f, (float) corner.y, (float) corner.x + 3.0f, (float) corner.y, 1.0f);
    }
}

void ClipOnizerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    // ---------------- TOP: назва + CLIP-лампа ----------------
    auto top = area.removeFromTop (70);
    clipIndicator.setBounds (top.removeFromLeft (70));

    auto titleArea = top;
    modelLabel.setBounds (titleArea.removeFromTop (26));
    subtitleLabel.setBounds (titleArea.removeFromTop (18));
    titleLabel.setBounds (titleArea);

    area.removeFromTop (8);

    // ---------------- BOTTOM: основні ручки ----------------
    auto bottom = area.removeFromBottom (150);
    const int knobWidth = bottom.getWidth() / 5;

    auto placeKnob = [] (juce::Rectangle<int> r, juce::Slider& s, juce::Label& l)
    {
        l.setBounds (r.removeFromTop (18));
        s.setBounds (r.reduced (8));
    };

    placeKnob (bottom.removeFromLeft (knobWidth), inputSlider, inputLabel);
    placeKnob (bottom.removeFromLeft (knobWidth), thresholdSlider, thresholdLabel);
    placeKnob (bottom.removeFromLeft (knobWidth), softnessSlider, softnessLabel);
    placeKnob (bottom.removeFromLeft (knobWidth), outputSlider, outputLabel);

    deltaButton.setBounds (bottom.reduced (14, 40));

    area.removeFromBottom (8);

    // ---------------- Смуга time-scale / BPM під осцилографом ----------------
    auto scopeRow = area.removeFromBottom (26);

    // ---------------- CENTER: два аналізатори ----------------
    auto zoomStrip = area.removeFromRight (26);
    area.removeFromRight (6);

    const int half = area.getWidth() / 2;
    auto shaperArea = area.removeFromLeft (half - 6);
    area.removeFromLeft (12);
    auto scopeArea = area;

    clipShaper.setBounds (shaperArea);
    oscilloscope.setBounds (scopeArea);
    verticalZoomSlider.setBounds (zoomStrip);

    // Time-scale кнопки під осцилографом (під правою половиною) + BPM
    auto scopeRowRight = scopeRow;
    scopeRowRight.setX (scopeArea.getX());
    scopeRowRight.setWidth (scopeArea.getWidth());

    auto timeScaleArea = scopeRowRight.removeFromLeft ((int) (scopeRowRight.getWidth() * 0.65f));
    const int btnW = timeScaleArea.getWidth() / 6;
    for (int i = 0; i < 6; ++i)
        timeScaleButtons[i].setBounds (timeScaleArea.removeFromLeft (btnW).reduced (2));

    bpmLabel.setBounds (scopeRowRight);
}
