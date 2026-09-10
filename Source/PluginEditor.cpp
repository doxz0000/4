/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    PluginEditor.cpp
*/

#include "PluginEditor.h"
#include <cmath>

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

void ClipOnizerLookAndFeel::drawRotarySlider (
    juce::Graphics& g, int x, int y, int width, int height,
    float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
    juce::Slider&)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (4.0f);
    const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

    juce::ColourGradient metalGrad (
        ClipOnizerColours::panelLight, centre.x, centre.y - radius,
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
// CLIP SHAPER
//==============================================================================
ClipShaperComponent::ClipShaperComponent (ClipOnizerAudioProcessor& p)
    : processor (p)
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
    const float knee01 = processor.getSoftnessNormalized();

    constexpr float maxAmp = 2.0f;
    auto ampToNorm = [maxAmp] (float amp)
    {
        return juce::jlimit (0.0f, 1.0f, amp / maxAmp);
    };

    g.setColour (ClipOnizerColours::scopeGrid);

    for (float db : { -24.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        const float norm = ampToNorm (juce::Decibels::decibelsToGain (db));
        const float x = plot.getX() + norm * plot.getWidth();
        const float y = plot.getBottom() - norm * plot.getHeight();

        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
    }

    g.setColour (ClipOnizerColours::traceNormal.withAlpha (0.3f));
    g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);

    constexpr int steps = 200;

    for (int i = 0; i < steps; ++i)
    {
        const float xAmpA = (i / (float) steps) * maxAmp;
        const float xAmpB = ((i + 1) / (float) steps) * maxAmp;

        const auto rA = ClipShaper::process (xAmpA, thresholdLin, knee01);
        const auto rB = ClipShaper::process (xAmpB, thresholdLin, knee01);

        const juce::Colour segColour =
            rA.second < 0.001f ? ClipOnizerColours::traceNormal
            : (rA.second < 0.85f ? ClipOnizerColours::traceSoftClip
                                  : ClipOnizerColours::traceHardClip);

        g.setColour (segColour);

        const float pxA = plot.getX() + ampToNorm (xAmpA) * plot.getWidth();
        const float pxB = plot.getX() + ampToNorm (xAmpB) * plot.getWidth();
        const float pyA = plot.getBottom() - ampToNorm (rA.first) * plot.getHeight();
        const float pyB = plot.getBottom() - ampToNorm (rB.first) * plot.getHeight();

        g.drawLine (pxA, pyA, pxB, pyB, 2.2f);
    }

    const float threshY =
        plot.getBottom() - ampToNorm (thresholdLin) * plot.getHeight();

    g.setColour (ClipOnizerColours::thresholdLine);

    float dash[2] = { 5.0f, 4.0f };

    g.drawDashedLine (
        juce::Line<float> (plot.getX(), threshY, plot.getRight(), threshY),
        dash, 2, 1.2f);

    const float thresholdDb = juce::Decibels::gainToDecibels (thresholdLin);

    g.setFont (juce::Font (
        juce::Font::getDefaultMonospacedFontName(), 12.0f,
        juce::Font::bold));

    g.drawText (
        (thresholdDb > 0.0f ? "THRESHOLD +" : "THRESHOLD ")
        + juce::String (thresholdDb, 1) + " dB",
        (int) plot.getX(), (int) threshY - 16, 200, 14,
        juce::Justification::left);

    const int64_t writeCounter =
        processor.scopeWriteCounter.load (std::memory_order_acquire);

    const int bufSize = ClipOnizerAudioProcessor::scopeBufferSize;

    const int lastIdx =
        static_cast<int> ((writeCounter - 1 + bufSize) % bufSize);

    const float liveAmp =
        writeCounter > 0
            ? std::abs (processor.scopeBuffer[(size_t) lastIdx].value)
            : 0.0f;

    liveDotSmoothed += (liveAmp - liveDotSmoothed) * 0.25f;

    const auto dotResult =
        ClipShaper::process (liveDotSmoothed, thresholdLin, knee01);

    const float dotPx =
        plot.getX() + ampToNorm (liveDotSmoothed) * plot.getWidth();

    const float dotPy =
        plot.getBottom() - ampToNorm (dotResult.first) * plot.getHeight();

    const juce::Colour dotColour =
        dotResult.second < 0.001f ? ClipOnizerColours::traceNormal
        : (dotResult.second < 0.85f ? ClipOnizerColours::traceSoftClip
                                    : ClipOnizerColours::traceHardClip);

    g.setColour (dotColour);
    g.fillEllipse (dotPx - 4.5f, dotPy - 4.5f, 9.0f, 9.0f);

    g.setColour (juce::Colours::white.withAlpha (0.6f));
    g.drawEllipse (dotPx - 4.5f, dotPy - 4.5f, 9.0f, 9.0f, 1.0f);

    g.setColour (ClipOnizerColours::textAmber.withAlpha (0.8f));
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText ("CLIP SHAPER",
                bounds.removeFromTop (18).toNearestInt(),
                juce::Justification::centred);

    g.setColour (ClipOnizerColours::metalEdge);
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 6.0f, 1.5f);
}

//==============================================================================
// OSCILLOSCOPE
//==============================================================================
OscilloscopeComponent::OscilloscopeComponent (ClipOnizerAudioProcessor& p)
    : processor (p)
{
    blockSamples.reserve (200000);
    previousBlockSamples.reserve (200000);

    lastCapturedCounter =
        processor.scopeWriteCounter.load (std::memory_order_acquire);

    startTimerHz (30);
}

void OscilloscopeComponent::setTimeDivision (float bars) noexcept
{
    barsOnScreen = juce::jlimit (0.25f, 8.0f, bars);

    // Changing the musical length starts a new capture cycle,
    // but the waveform currently visible on screen is preserved.
    previousBlockSamples = blockSamples;

    previousBlockId = currentBlockId;

    blockSamples.clear();

    currentBlockId =
        std::numeric_limits<int64_t>::min();

    const int64_t writeCounter =
        processor.scopeWriteCounter.load (
            std::memory_order_acquire);

    lastCapturedCounter = writeCounter;

    // Tell the audio thread to create a fresh musical block.
    processor.setOscilloscopeBars (barsOnScreen);
}

void OscilloscopeComponent::setVerticalZoom (float zoom) noexcept
{
    verticalZoom = juce::jlimit (0.2f, 4.0f, zoom);
}











void OscilloscopeComponent::captureNewSamples()
{
    const int64_t blockId =
        processor.scopeBlockId.load (
            std::memory_order_acquire);

    const int64_t blockStart =
        processor.scopeBlockStartCounter.load (
            std::memory_order_acquire);

    const int64_t writeCounter =
        processor.scopeWriteCounter.load (
            std::memory_order_acquire);

    if (blockId == std::numeric_limits<int64_t>::min()
        || writeCounter <= 0)
        return;

    // -------------------------------------------------------------------------
    // NEW MUSICAL BLOCK
    //
    // Do NOT clear the waveform that is currently visible.
    //
    // Instead:
    //     current block -> previous block
    //     new block     -> starts empty
    //
    // This allows the previous waveform to remain visible while the
    // new waveform is progressively drawn over it.
    // -------------------------------------------------------------------------

    if (currentBlockId != blockId)
    {
        if (currentBlockId != std::numeric_limits<int64_t>::min()
            && ! blockSamples.empty())
        {
            previousBlockSamples = blockSamples;
            previousBlockId = currentBlockId;
        }

        currentBlockId = blockId;

        blockSamples.clear();

        lastCapturedCounter = blockStart;
    }

    // -------------------------------------------------------------------------
    // COPY NEW SAMPLES
    // -------------------------------------------------------------------------

    const int64_t blockIdBeforeCopy = blockId;

    if (lastCapturedCounter < blockStart)
        lastCapturedCounter = blockStart;

    int64_t available =
        writeCounter - lastCapturedCounter;

    if (available <= 0)
        return;

    // The ring buffer contains only the latest scopeBufferSize samples.
    if (available > ClipOnizerAudioProcessor::scopeBufferSize)
    {
        lastCapturedCounter =
            writeCounter
            - ClipOnizerAudioProcessor::scopeBufferSize;

        available =
            ClipOnizerAudioProcessor::scopeBufferSize;

        blockSamples.clear();
    }

    const size_t maxStored =
        static_cast<size_t> (
            ClipOnizerAudioProcessor::scopeBufferSize);

    for (int64_t n = 0; n < available; ++n)
    {
        const int64_t absoluteIndex =
            lastCapturedCounter + n;

        const int index =
            static_cast<int> (
                absoluteIndex
                % ClipOnizerAudioProcessor::scopeBufferSize);

        blockSamples.push_back (
            processor.scopeBuffer[
                static_cast<size_t> (index)]);
    }

    lastCapturedCounter = writeCounter;

    // -------------------------------------------------------------------------
    // CHECK IF BLOCK CHANGED DURING COPY
    // -------------------------------------------------------------------------

    const int64_t blockIdAfterCopy =
        processor.scopeBlockId.load (
            std::memory_order_acquire);

    if (blockIdAfterCopy != blockIdBeforeCopy)
    {
        // Keep the completed block as the previous waveform.
        if (! blockSamples.empty())
        {
            previousBlockSamples = blockSamples;
            previousBlockId = blockIdBeforeCopy;
        }

        blockSamples.clear();

        currentBlockId = blockIdAfterCopy;

        lastCapturedCounter =
            processor.scopeBlockStartCounter.load (
                std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // LIMIT MEMORY
    // -------------------------------------------------------------------------

    if (blockSamples.size() > maxStored)
    {
        blockSamples.erase (
            blockSamples.begin(),
            blockSamples.begin()
                + static_cast<ptrdiff_t> (
                    blockSamples.size() - maxStored));
    }

    if (previousBlockSamples.size() > maxStored)
    {
        previousBlockSamples.erase (
            previousBlockSamples.begin(),
            previousBlockSamples.begin()
                + static_cast<ptrdiff_t> (
                    previousBlockSamples.size() - maxStored));
    }
}









void OscilloscopeComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (ClipOnizerColours::scopeGlassBg);
    g.fillRoundedRectangle (bounds, 6.0f);

    auto plot = bounds.reduced (26.0f, 22.0f);

    const double bpm =
        juce::jmax (20.0,
                    processor.currentBpm.load (std::memory_order_acquire));

    const double qnPerBar =
        juce::jmax (0.25,
                    processor.currentQuarterNotesPerBar.load (
                        std::memory_order_acquire));

    constexpr float baseRange = 2.0f;

    const float effectiveRange =
        juce::jmax (
            0.05f,
            baseRange / juce::jmax (0.1f, verticalZoom));

    auto ampToY = [&] (float amp)
    {
        const float norm =
            juce::jlimit (-1.0f, 1.0f, amp / effectiveRange);

        return plot.getCentreY()
               - norm * (plot.getHeight() * 0.5f);
    };

    // dB grid
    for (float db : { -24.0f, -12.0f, -6.0f, 0.0f, 6.0f })
    {
        const float y =
            ampToY (juce::Decibels::decibelsToGain (db));

        if (y >= plot.getY() && y <= plot.getBottom())
        {
            g.setColour (ClipOnizerColours::scopeGrid);

            g.drawHorizontalLine (
                static_cast<int> (y),
                plot.getX(), plot.getRight());

            g.setColour (
                ClipOnizerColours::textAmber.withAlpha (0.55f));

            g.setFont (10.0f);

            g.drawText (
                (db > 0.0f ? "+" : juce::String())
                    + juce::String (db, 0) + " dB",
                static_cast<int> (plot.getX()) + 3,
                static_cast<int> (y) - 12,
                55, 12,
                juce::Justification::left);
        }
    }

    // Musical grid. No playhead access from UI.
    const double totalQuarterNotes =
        qnPerBar * static_cast<double> (barsOnScreen);

    const int quarterLineCount =
        juce::jmax (
            1,
            static_cast<int> (std::ceil (totalQuarterNotes)));

    for (int q = 0; q <= quarterLineCount; ++q)
    {
        const float x =
            plot.getX()
            + (static_cast<float> (q)
               / static_cast<float> (quarterLineCount))
              * plot.getWidth();

        const bool isBarLine =
            std::fmod (static_cast<double> (q), qnPerBar) < 0.001;

        g.setColour (
            isBarLine
                ? ClipOnizerColours::textAmber.withAlpha (0.28f)
                : ClipOnizerColours::scopeGrid);

        g.drawVerticalLine (
            static_cast<int> (x),
            plot.getY(), plot.getBottom());
    }








    // -------------------------------------------------------------------------
    // WAVEFORM
    //
    // Previous block stays visible.
    // Current block progressively overwrites it from left to right.
    // -------------------------------------------------------------------------

    const int widthPx =
        static_cast<int> (plot.getWidth());

    if (widthPx > 0)
    {
        const float thresholdLin =
            processor.getThresholdLinear();

        const float knee01 =
            processor.getSoftnessNormalized();

        const float kneeWidth =
            knee01 > 0.001f
                ? juce::jmax (0.0005f, knee01)
                : 0.0f;

        const float kneeStart =
            kneeWidth > 0.0f
                ? juce::jmax (
                    0.0f,
                    thresholdLin - kneeWidth)
                : thresholdLin;

        // ---------------------------------------------------------------------
        // Expected size of one complete musical block.
        // ---------------------------------------------------------------------

        const double sampleRate =
            processor.getSampleRate() > 0.0
                ? processor.getSampleRate()
                : 44100.0;

        const double expectedBlockSamples =
            juce::jmax (
                1.0,
                (60.0 / bpm)
                * sampleRate
                * totalQuarterNotes);

        // ---------------------------------------------------------------------
        // Calculate how much of the NEW waveform has already arrived.
        //
        // 0.0 = just started
        // 1.0 = complete block
        // ---------------------------------------------------------------------

        const float progress =
            juce::jlimit (
                0.0f,
                1.0f,
                static_cast<float> (
                    static_cast<double> (blockSamples.size())
                    / expectedBlockSamples));

        const int newVisibleWidth =
            juce::jlimit (
                0,
                widthPx,
                static_cast<int> (
                    progress
                    * static_cast<float> (widthPx)));

        
        
        
                auto drawWaveform =
            [&] (const std::vector<ScopeSample>& samples,
                 int endPx,
                 double referenceTotalSamples)
        {
            if (samples.empty())
                return;

            if (endPx <= 0)
                return;

            const size_t availableSamples = samples.size();

            // ВАЖНО: маппинг пиксель -> сэмпл всегда идёт от ФИКСИРОВАННОГО
            // референсного количества сэмплов (ожидаемая длина всего
            // музыкального блока), а НЕ от samples.size().
            //
            // samples.size() растёт каждый кадр, пока блок ещё пишется,
            // из-за этого раньше пересчитывался масштаб на каждый repaint —
            // это и давало эффект "плывёт/сжимается", а потом
            // "устаканивается", а также мелкий дрейф волны относительно
            // сетки на каждом проходе. С фиксированным референсом
            // пиксель X с первого кадра соответствует одному и тому же
            // абсолютному сэмплу от начала блока и никогда не смещается.
            const double totalForMapping =
                juce::jmax (1.0, referenceTotalSamples);

            for (int px = 0; px < endPx; ++px)
            {
                const size_t sampleA = static_cast<size_t> (
                    (static_cast<double> (px) / static_cast<double> (widthPx))
                    * totalForMapping);

                if (sampleA >= availableSamples)
                    continue;

                size_t sampleB = static_cast<size_t> (
                    (static_cast<double> (px + 1) / static_cast<double> (widthPx))
                    * totalForMapping);

                sampleB = juce::jmax (sampleB, sampleA + static_cast<size_t> (1));
                sampleB = juce::jmin (sampleB, availableSamples);

                float minV = 1.0e6f;
                float maxV = -1.0e6f;

                bool hasYellow = false;
                bool hasRed = false;

                for (size_t s = sampleA; s < sampleB; ++s)
                {
                    const auto& sample = samples[s];
                    const float value = sample.value;
                    const float absValue = std::abs (value);

                    minV = juce::jmin (minV, value);
                    maxV = juce::jmax (maxV, value);

                    if (sample.clipAmount >= 0.85f)
                        hasRed = true;
                    else if (sample.clipAmount > 0.001f)
                        hasYellow = true;

                    if (absValue > thresholdLin)
                        hasRed = true;

                    if (kneeWidth > 0.0f
                        && absValue > kneeStart
                        && absValue <= thresholdLin)
                    {
                        hasYellow = true;
                    }
                }

                juce::Colour traceColour;

                if (hasRed)
                    traceColour = ClipOnizerColours::traceHardClip;
                else if (hasYellow)
                    traceColour = ClipOnizerColours::traceSoftClip;
                else
                    traceColour = ClipOnizerColours::traceNormal;

                g.setColour (traceColour);

                const float x =
                    plot.getX()
                    + static_cast<float> (px)
                    / static_cast<float> (widthPx)
                    * plot.getWidth();

                const float y1 = ampToY (minV);
                const float y2 = ampToY (maxV);

                g.drawLine (x, y1, x, y2, 2.0f);
            }
        };

        // 1. Предыдущий (уже завершённый и неизменный) блок — рисуем
        //    на всю ширину, используя его собственный (статичный) размер.
        if (! previousBlockSamples.empty())
        {
            drawWaveform (previousBlockSamples,
                          widthPx,
                          static_cast<double> (previousBlockSamples.size()));
        }

        // 2. Новый (ещё заполняющийся) блок — рисуем поверх, используя
        //    ФИКСИРОВАННЫЙ expectedBlockSamples как референс.
        if (newVisibleWidth > 0 && ! blockSamples.empty())
        {
            drawWaveform (blockSamples,
                          newVisibleWidth,
                          expectedBlockSamples);
        }
    }    
        


    // Threshold lines
    const float thresholdLin =
        processor.getThresholdLinear();

    const float knee01 =
        processor.getSoftnessNormalized();

    const float kneeWidth =
        knee01 > 0.001f ? juce::jmax (0.0005f, knee01) : 0.0f;

    const float kneeStart =
        kneeWidth > 0.0f
            ? juce::jmax (0.0f, thresholdLin - kneeWidth)
            : thresholdLin;

    const float thresholdY = ampToY (thresholdLin);
    const float negativeThresholdY = ampToY (-thresholdLin);

    g.setColour (ClipOnizerColours::thresholdLine);

    float dash[2] = { 5.0f, 4.0f };

    g.drawDashedLine (
        juce::Line<float> (
            plot.getX(), thresholdY,
            plot.getRight(), thresholdY),
        dash, 2, 1.5f);

    g.drawDashedLine (
        juce::Line<float> (
            plot.getX(), negativeThresholdY,
            plot.getRight(), negativeThresholdY),
        dash, 2, 1.5f);

    if (kneeWidth > 0.0f && kneeStart < thresholdLin)
    {
        const float kneePositiveY = ampToY (kneeStart);
        const float kneeNegativeY = ampToY (-kneeStart);

        g.setColour (
            ClipOnizerColours::traceSoftClip.withAlpha (0.45f));

        float kneeDash[2] = { 3.0f, 5.0f };

        g.drawDashedLine (
            juce::Line<float> (
                plot.getX(), kneePositiveY,
                plot.getRight(), kneePositiveY),
            kneeDash, 2, 1.0f);

        g.drawDashedLine (
            juce::Line<float> (
                plot.getX(), kneeNegativeY,
                plot.getRight(), kneeNegativeY),
            kneeDash, 2, 1.0f);
    }

    const float thresholdDb =
        juce::Decibels::gainToDecibels (thresholdLin);

    g.setFont (juce::Font (
        juce::Font::getDefaultMonospacedFontName(),
        12.0f, juce::Font::bold));

    g.setColour (ClipOnizerColours::thresholdLine);

    g.drawText (
        (thresholdDb > 0.0f ? "THRESHOLD +" : "THRESHOLD ")
        + juce::String (thresholdDb, 1) + " dB",
        static_cast<int> (plot.getX()),
        static_cast<int> (thresholdY) - 16,
        200, 14,
        juce::Justification::left);

    // Progress indicator.
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

    g.setColour (
        ClipOnizerColours::textAmber.withAlpha (0.65f));

    g.setFont (juce::Font (
        juce::Font::getDefaultMonospacedFontName(),
        10.0f, juce::Font::bold));

    g.drawText (
        juce::String (barsOnScreen, 2) + " BAR   "
            + juce::String (percent) + "%",
        static_cast<int> (plot.getRight()) - 120,
        static_cast<int> (plot.getY()) - 17,
        120, 14,
        juce::Justification::right);

    g.setColour (
        ClipOnizerColours::textAmber.withAlpha (0.8f));

    g.setFont (juce::Font (12.0f, juce::Font::bold));

    g.drawText (
        "REAL-TIME OSCILLOSCOPE",
        bounds.removeFromTop (18).toNearestInt(),
        juce::Justification::centred);

    g.setColour (ClipOnizerColours::metalEdge);

    g.drawRoundedRectangle (
        getLocalBounds().toFloat().reduced (1.0f),
        6.0f, 1.5f);
}



//==============================================================================
// CLIP INDICATOR
//==============================================================================
ClipIndicatorComponent::ClipIndicatorComponent (ClipOnizerAudioProcessor& p)
    : processor (p)
{
    startTimerHz (30);
}

void ClipIndicatorComponent::paint (juce::Graphics& g)
{
    const float target =
        processor.clipIndicatorLevel.load (std::memory_order_acquire);

    smoothedLevel += (target - smoothedLevel) * 0.2f;

    auto bounds = getLocalBounds().toFloat().reduced (6.0f);
    auto ledArea = bounds.withHeight (bounds.getHeight() - 14.0f);
    const auto centre = ledArea.getCentre();
    const float radius =
        juce::jmin (ledArea.getWidth(), ledArea.getHeight()) * 0.5f;

    juce::Colour ledColour;

    if (smoothedLevel < 0.05f)
        ledColour = juce::Colours::green.darker (0.3f);
    else if (smoothedLevel < 0.3f)
        ledColour = juce::Colour::fromHSV (
            juce::jmap (smoothedLevel, 0.05f, 0.3f, 0.33f, 0.16f),
            0.9f, 1.0f, 1.0f);
    else if (smoothedLevel < 0.65f)
        ledColour = juce::Colour::fromHSV (
            juce::jmap (smoothedLevel, 0.3f, 0.65f, 0.16f, 0.02f),
            0.95f, 1.0f, 1.0f);
    else
        ledColour = juce::Colours::red.brighter (
            juce::jmin (0.4f, smoothedLevel - 0.65f));

    juce::ColourGradient glow (
        ledColour.withAlpha (0.85f), centre.x, centre.y,
        ledColour.withAlpha (0.0f),
        centre.x, centre.y - radius * 2.4f, false);

    g.setGradientFill (glow);

    g.fillEllipse (
        centre.x - radius * 1.9f,
        centre.y - radius * 1.9f,
        radius * 3.8f,
        radius * 3.8f);

    g.setColour (ClipOnizerColours::ledOff);
    g.fillEllipse (ledArea);

    g.setColour (ledColour);
    g.fillEllipse (ledArea.reduced (radius * 0.15f));

    g.setColour (juce::Colours::white.withAlpha (0.25f));
    g.drawEllipse (ledArea, 1.5f);

    g.setColour (ClipOnizerColours::textAmber);
    g.setFont (juce::Font (10.0f, juce::Font::bold));
    g.drawText (
        "CLIP",
        bounds.removeFromBottom (14.0f).toNearestInt(),
        juce::Justification::centred);
}

//==============================================================================
// EDITOR
//==============================================================================
ClipOnizerAudioProcessorEditor::ClipOnizerAudioProcessorEditor (
    ClipOnizerAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      clipShaper (p),
      oscilloscope (p),
      clipIndicator (p)
{
    setLookAndFeel (&lookAndFeel);

    auto setupKnob =
        [this] (juce::Slider& s,
                juce::Label& l,
                const juce::String& text)
    {
        s.setSliderStyle (
            juce::Slider::RotaryHorizontalVerticalDrag);

        s.setTextBoxStyle (
            juce::Slider::TextBoxBelow,
            false, 80, 16);

        s.setRotaryParameters (
            juce::MathConstants<float>::pi * 1.2f,
            juce::MathConstants<float>::pi * 2.8f,
            true);

        s.setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (s);

        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::Font (13.0f, juce::Font::bold));

        addAndMakeVisible (l);
    };

    setupKnob (inputSlider, inputLabel, "INPUT");
    setupKnob (thresholdSlider, thresholdLabel, "THRESHOLD");
    setupKnob (softnessSlider, softnessLabel, "SOFTNESS / KNEE");
    setupKnob (outputSlider, outputLabel, "OUTPUT");

    inputAttachment =
        std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>
        (audioProcessor.apvts,
         ClipOnizerAudioProcessor::idInput,
         inputSlider);

    thresholdAttachment =
        std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>
        (audioProcessor.apvts,
         ClipOnizerAudioProcessor::idThreshold,
         thresholdSlider);

    softnessAttachment =
        std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>
        (audioProcessor.apvts,
         ClipOnizerAudioProcessor::idSoftness,
         softnessSlider);

    outputAttachment =
        std::make_unique<
            juce::AudioProcessorValueTreeState::SliderAttachment>
        (audioProcessor.apvts,
         ClipOnizerAudioProcessor::idOutput,
         outputSlider);

    deltaButton.setClickingTogglesState (true);

    addAndMakeVisible (deltaButton);

    deltaAttachment =
        std::make_unique<
            juce::AudioProcessorValueTreeState::ButtonAttachment>
        (audioProcessor.apvts,
         ClipOnizerAudioProcessor::idDelta,
         deltaButton);

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

        timeScaleButtons[i].onClick =
            [this, i] { timeScaleButtonClicked (i); };

        addAndMakeVisible (timeScaleButtons[i]);
    }

    timeScaleButtons[2].setToggleState (
        true, juce::dontSendNotification);

    bpmLabel.setJustificationType (
        juce::Justification::centredRight);

    bpmLabel.setFont (
        juce::Font (
            juce::Font::getDefaultMonospacedFontName(),
            13.0f, juce::Font::bold));

    addAndMakeVisible (bpmLabel);

    verticalZoomSlider.setSliderStyle (
        juce::Slider::LinearVertical);

    verticalZoomSlider.setRange (0.2, 4.0, 0.01);
    verticalZoomSlider.setValue (1.0);
    verticalZoomSlider.setTextBoxStyle (
        juce::Slider::NoTextBox, false, 0, 0);

    verticalZoomSlider.onValueChange =
        [this]
        {
            oscilloscope.setVerticalZoom (
                static_cast<float> (
                    verticalZoomSlider.getValue()));
        };

    addAndMakeVisible (verticalZoomSlider);

    modelLabel.setText (
        "MODEL-CLIPONIZER No.8",
        juce::dontSendNotification);

    modelLabel.setFont (
        juce::Font (17.0f, juce::Font::bold));

    modelLabel.setJustificationType (
        juce::Justification::centred);

    addAndMakeVisible (modelLabel);

    subtitleLabel.setText (
        "DR.DOC SOUNDLAB-EQUIPMENT",
        juce::dontSendNotification);

    subtitleLabel.setFont (juce::Font (11.0f));
    subtitleLabel.setJustificationType (
        juce::Justification::centred);

    addAndMakeVisible (subtitleLabel);

    titleLabel.setText (
        "CLIP-TO-ZERO   /   NO LATENCY CLIPPER",
        juce::dontSendNotification);

    titleLabel.setFont (
        juce::Font (13.0f, juce::Font::italic));

    titleLabel.setJustificationType (
        juce::Justification::centred);

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
    bpmLabel.setText (
        "BPM: "
        + juce::String (
            static_cast<int> (
                std::round (
                    audioProcessor.currentBpm.load (
                        std::memory_order_acquire)))),
        juce::dontSendNotification);
}

void ClipOnizerAudioProcessorEditor::timeScaleButtonClicked (int index)
{
    if (index < 0 || index >= 6)
        return;

    oscilloscope.setTimeDivision (
        timeScaleValues[index]);
}

//==============================================================================
void ClipOnizerAudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ColourGradient bg (
        ClipOnizerColours::panelLight, 0.0f, 0.0f,
        ClipOnizerColours::panelDark, 0.0f,
        static_cast<float> (getHeight()), false);

    g.setGradientFill (bg);
    g.fillAll();

    g.setColour (ClipOnizerColours::metalEdge);
    g.drawRect (getLocalBounds(), 2);

    for (auto corner :
         { juce::Point<int> (14, 14),
           juce::Point<int> (getWidth() - 14, 14),
           juce::Point<int> (14, getHeight() - 14),
           juce::Point<int> (getWidth() - 14, getHeight() - 14) })
    {
        g.setColour (ClipOnizerColours::metalEdge.darker());
        g.fillEllipse (
            static_cast<float> (corner.x) - 5.0f,
            static_cast<float> (corner.y) - 5.0f,
            10.0f, 10.0f);

        g.setColour (juce::Colours::black.withAlpha (0.5f));

        g.drawLine (
            static_cast<float> (corner.x) - 3.0f,
            static_cast<float> (corner.y),
            static_cast<float> (corner.x) + 3.0f,
            static_cast<float> (corner.y),
            1.0f);
    }
}

void ClipOnizerAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto top = area.removeFromTop (70);

    clipIndicator.setBounds (
        top.removeFromLeft (70));

    auto titleArea = top;

    modelLabel.setBounds (
        titleArea.removeFromTop (26));

    subtitleLabel.setBounds (
        titleArea.removeFromTop (18));

    titleLabel.setBounds (titleArea);

    area.removeFromTop (8);

    auto bottom = area.removeFromBottom (150);

    const int knobWidth =
        bottom.getWidth() / 5;

    auto placeKnob =
        [] (juce::Rectangle<int> r,
            juce::Slider& s,
            juce::Label& l)
    {
        l.setBounds (
            r.removeFromTop (18));

        s.setBounds (
            r.reduced (8));
    };

    placeKnob (
        bottom.removeFromLeft (knobWidth),
        inputSlider, inputLabel);

    placeKnob (
        bottom.removeFromLeft (knobWidth),
        thresholdSlider, thresholdLabel);

    placeKnob (
        bottom.removeFromLeft (knobWidth),
        softnessSlider, softnessLabel);

    placeKnob (
        bottom.removeFromLeft (knobWidth),
        outputSlider, outputLabel);

    deltaButton.setBounds (
        bottom.reduced (14, 40));

    area.removeFromBottom (8);

    auto scopeRow =
        area.removeFromBottom (26);

    auto zoomStrip =
        area.removeFromRight (26);

    area.removeFromRight (6);

    const int half =
        area.getWidth() / 2;

    auto shaperArea =
        area.removeFromLeft (half - 6);

    area.removeFromLeft (12);

    auto scopeArea = area;

    clipShaper.setBounds (shaperArea);
    oscilloscope.setBounds (scopeArea);
    verticalZoomSlider.setBounds (zoomStrip);

    auto scopeRowRight = scopeRow;

    scopeRowRight.setX (scopeArea.getX());
    scopeRowRight.setWidth (scopeArea.getWidth());

    auto timeScaleArea =
        scopeRowRight.removeFromLeft (
            static_cast<int> (
                scopeRowRight.getWidth() * 0.65f));

    const int btnW =
        juce::jmax (1, timeScaleArea.getWidth() / 6);

    for (int i = 0; i < 6; ++i)
    {
        timeScaleButtons[i].setBounds (
            timeScaleArea.removeFromLeft (btnW)
                .reduced (2));
    }

    bpmLabel.setBounds (scopeRowRight);
}
