/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    CLIP-TO-ZERO / No Latency Clipper

    Цей файл БЕЗ ЗМІН відносно оригіналу — DSP-логіка і оброблення
    аудіо не залежать від того, чим намальований інтерфейс (JUCE-компоненти
    чи HTML/CSS у WebBrowserComponent). Усі поля, потрібні редактору
    (scopeBuffer, scopeWriteCounter, currentBpm, apvts, і т.д.), вже public.
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <utility>
#include <cstdint>

struct ScopeSample
{
    float value = 0.0f;
    float clipAmount = 0.0f;
    float out = 0.0f;   // сигнал ПІСЛЯ сатуратора + кліпера (без output gain / delta)
};

class ClipShaper
{
public:
    static std::pair<float, float> process (float x, float thresholdLin, float knee01) noexcept
    {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float absX = std::abs (x);

        if (knee01 < 0.001f)
        {
            if (absX <= thresholdLin)
                return { x, 0.0f };

            return { sign * thresholdLin, 1.0f };
        }

        const float kneeWidth = juce::jmax (0.0005f, knee01);
        const float kneeStart = juce::jmax (0.0f, thresholdLin - kneeWidth);

        if (absX <= kneeStart)
            return { x, 0.0f };

        const float overshoot = absX - kneeStart;
        const float shaped = kneeWidth * std::tanh (overshoot / kneeWidth);
        const float outAbs = kneeStart + shaped;
        const float clipAmount = juce::jlimit (0.0f, 1.0f, shaped / kneeWidth);

        return { sign * outAbs, clipAmount };
    }
};

//==============================================================================
// SATURATOR — analog-style whole-curve waveshaper, placed BEFORE the clipper.
//
// Works in a threshold-normalised domain u = x / T:
//     s(u) = (tanh(a(u+b)) - tanh(ab)) / D,   D chosen so |s(±1)| <= 1
// - quiet signal gets up to a/tanh(a) more level (denser, "glued" low end),
// - the gain falls smoothly to x1 at the threshold and below x1 above it,
//   so the saturator never pushes anything over the threshold,
// - bias b adds a little asymmetry -> 2nd harmonic (tube/tape warmth).
//
// Anti-aliasing: 1st-order ADAA applied ONLY to the nonlinear residual
// r(u) = s(u) - u. The clean (linear) part passes untouched, so there is
// no latency and no high-frequency loss; only the harmonics are smoothed.
// A ~5 Hz DC blocker on the residual removes DC introduced by the bias.
//==============================================================================
class Saturator
{
public:
    struct Coeffs
    {
        double a = 0.0;     // drive
        double b = 0.0;     // bias (asymmetry)
        double tab = 0.0;   // tanh(a*b)
        double invD = 1.0;  // normalisation
        bool active = false;
    };

    static Coeffs makeCoeffs (float amount01) noexcept
    {
        Coeffs c;
        const double amt = juce::jlimit (0.0, 1.0, (double) amount01);
        c.a = 3.0 * std::pow (amt, 1.3);

        if (c.a < 1.0e-4)
            return c; // inactive -> identity

        // Bias defined in the drive domain (a*b) so asymmetry stays subtle:
        // ~-33 dB 2nd harmonic at full drive, positive peak ~0.89*T.
        c.b = (0.06 * amt) / c.a;
        c.active = true;
        c.tab = std::tanh (c.a * c.b);
        const double dPos = std::tanh (c.a * (1.0 + c.b)) - c.tab;
        const double dNeg = c.tab - std::tanh (c.a * (c.b - 1.0));
        c.invD = 1.0 / juce::jmax (1.0e-12, juce::jmax (dPos, dNeg));
        return c;
    }

    // Static curve (no ADAA) — also mirrored in ui.js for the shaper display.
    static double shape (double u, const Coeffs& c) noexcept
    {
        return (std::tanh (c.a * (u + c.b)) - c.tab) * c.invD;
    }

    void prepare (double sampleRate) noexcept
    {
        dcCoeff = std::exp (-2.0 * juce::MathConstants<double>::pi * 5.0
                            / juce::jmax (1.0, sampleRate));
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : state)
            s = {};
    }

    // Keeps ADAA history in sync while bypassed so re-engaging is click-free.
    void trackBypassed (int ch, float x) noexcept
    {
        auto& s = state[(size_t) juce::jlimit (0, maxChannels - 1, ch)];
        s.x1 = x;
        s.R1Valid = false;
        s.dcX1 = 0.0;
        s.dcY1 = 0.0;
    }

    // paramsChanging: true while drive/threshold are being smoothed —
    // forces the previous antiderivative to be recomputed with current coeffs.
    float process (int ch, float x, float thresholdLin, const Coeffs& c,
                   bool paramsChanging) noexcept
    {
        auto& s = state[(size_t) juce::jlimit (0, maxChannels - 1, ch)];

        if (! c.active)
        {
            trackBypassed (ch, x);
            return x;
        }

        const double T = juce::jmax (1.0e-6, (double) thresholdLin);
        const double invT = 1.0 / T;
        const double u  = (double) x * invT;
        const double u1 = s.x1 * invT;

        const double R0 = antiderivative (u, c);

        if (paramsChanging || ! s.R1Valid)
            s.R1 = antiderivative (u1, c);

        const double du = u - u1;
        const double res = std::abs (du) > 1.0e-5
                         ? (R0 - s.R1) / du
                         : residual (0.5 * (u + u1), c);

        s.x1 = (double) x;
        s.R1 = R0;
        s.R1Valid = true;

        // DC blocker on the residual only.
        const double y = res - s.dcX1 + dcCoeff * s.dcY1;
        s.dcX1 = res;
        s.dcY1 = y;

        return (float) ((double) x + T * y);
    }

private:
    static constexpr int maxChannels = 2;

    struct ChannelState
    {
        double x1 = 0.0;
        double R1 = 0.0;
        bool R1Valid = false;
        double dcX1 = 0.0;
        double dcY1 = 0.0;
    };

    static double logCosh (double z) noexcept
    {
        const double az = std::abs (z);
        return az + std::log1p (std::exp (-2.0 * az)) - 0.69314718055994530942; // ln(2)
    }

    static double residual (double u, const Coeffs& c) noexcept
    {
        return shape (u, c) - u;
    }

    // d/du R(u) = residual(u)
    static double antiderivative (double u, const Coeffs& c) noexcept
    {
        return (logCosh (c.a * (u + c.b)) / c.a - u * c.tab) * c.invD - 0.5 * u * u;
    }

    std::array<ChannelState, maxChannels> state {};
    double dcCoeff = 0.9995;
};

class ClipOnizerAudioProcessor : public juce::AudioProcessor
{
public:
    ClipOnizerAudioProcessor();
    ~ClipOnizerAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static constexpr const char* idInput     = "input";
    static constexpr const char* idOutput    = "output";
    static constexpr const char* idThreshold = "threshold";
    static constexpr const char* idSoftness  = "softness";
    static constexpr const char* idDelta     = "delta";
    static constexpr const char* idSaturator = "saturator";

    // -------------------------------------------------------------------------
    // OSCILLOSCOPE DATA
    //
    // Absolute counter is intentionally used instead of a modulo-only write
    // position. A modulo position cannot tell whether the ring buffer wrapped.
    static constexpr int scopeBufferSize = 1 << 20; // 1,048,576 samples
    std::array<ScopeSample, scopeBufferSize> scopeBuffer {};

    std::atomic<int64_t> scopeWriteCounter { 0 };

    // Current musical block and the absolute counter where it started.
    std::atomic<int64_t> scopeBlockId { std::numeric_limits<int64_t>::min() };
    std::atomic<int64_t> scopeBlockStartCounter { 0 };

    // Unique transport cycle.
    // Changes every time the DAW crosses the loop boundary.
    std::atomic<int64_t> scopeLoopCycle { 0 };

    // Current host information, written only from processBlock().
    std::atomic<double> currentBpm { 120.0 };
    std::atomic<double> currentQuarterNotesPerBar { 4.0 };

    // Selected oscilloscope length: 0.25 / 0.5 / 1 / 2 / 4 / 8 bars.
    std::atomic<float> oscilloscopeBars { 1.0f };
    std::atomic<uint32_t> oscilloscopeConfigVersion { 0 };

    // Fallback transport when PPQ is unavailable.
    double fallbackPpq = 0.0;

    std::atomic<float> clipIndicatorLevel { 0.0f };
    std::atomic<float> scopeLivePeakPos { 0.0f };   // огинаюча позитивних піків
    std::atomic<float> scopeLivePeakNeg { 0.0f };   // огинаюча негативних піків (значення <= 0) // <-- нове: піковий рівень для точки на Clip Shaper

    void setOscilloscopeBars (float bars) noexcept
    {
        bars = juce::jlimit (0.25f, 8.0f, bars);
        oscilloscopeBars.store (bars, std::memory_order_release);
        oscilloscopeConfigVersion.fetch_add (1, std::memory_order_acq_rel);
    }

    float getOscilloscopeBars() const noexcept
    {
        return oscilloscopeBars.load (std::memory_order_acquire);
    }

    float getThresholdLinear() const noexcept;
    float getSoftnessNormalized() const noexcept;

private:
    juce::LinearSmoothedValue<float> inputGainSmoothed { 1.0f };
    juce::LinearSmoothedValue<float> outputGainSmoothed { 1.0f };
    juce::LinearSmoothedValue<float> saturatorSmoothed { 0.0f };      // 0..1
    juce::LinearSmoothedValue<float> satThresholdSmoothed { 1.0f };   // linear

    Saturator saturator;

    float clipEnvelope = 0.0f;
    float livePeakPosEnvelope = 0.0f;
    float livePeakNegEnvelope = 0.0f;
    uint32_t lastScopeConfigVersion = 0;
    int livePeakPosHoldCounter = 0;
    int livePeakNegHoldCounter = 0;

    // Состояние детектора луп-цикла — ДОЛЖНО быть per-instance,
    // а не static thread_local (иначе несколько инстансов плагина
    // на одном аудио-потоке будут ломать друг другу loop-detection).
    double scopeLoopPreviousPpq = 0.0;
    bool scopeLoopPreviousIsLooping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessor)
};
