/*
    MODEL-CLIPONIZER No.8 — DR.DOC SOUNDLAB-EQUIPMENT
    CLIP-TO-ZERO / No Latency Clipper

    PluginProcessor.h
    ------------------
    Тут живе увесь DSP та дані, які потрібні редактору (UI) для малювання
    двох аналізаторів (Clip Shaper + Oscilloscope) і CLIP-індикатора.

    ПРИНЦИП ZERO-LATENCY:
    Основний clipping-path обробляє сигнал вибірка-за-вибіркою,
    без lookahead і без будь-якого буферування, що затримує аудіо.
    Дані для UI лише *читаються* з кільцевого буфера, який пишеться
    в processBlock() — це не додає жодного latency до аудіотракту.
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <utility>

//==============================================================================
// Один семпл, який ми віддаємо в UI для осцилографа.
// value       — амплітуда сигналу (той самий сигнал, що йде далі по ланцюгу:
//               clipped-сигнал, або delta-сигнал, якщо увімкнено DELTA);
// clipAmount  — 0 = нормальний сигнал, (0..1) = у soft-knee зоні,
//               1 = повний hard-clip. Використовується лише для кольору.
struct ScopeSample
{
    float value = 0.0f;
    float clipAmount = 0.0f;
};

//==============================================================================
// Чиста математика кліпера. Викликається і в audio-thread (processBlock),
// і в message-thread (для малювання transfer curve в Clip Shaper) —
// тому це static-функція без стану, безпечна для викликів з будь-якого потоку.
class ClipShaper
{
public:
    // x            — вхідний сигнал (лінійна амплітуда, зазвичай -2..2)
    // thresholdLin — порогове значення в лінійних одиницях (dB -> gain),
    //                може бути > 1.0, якщо Threshold виставлено вище 0 dB
    // knee01       — softness, нормалізований 0..1 (0 = hard clip)
    //
    // Повертає { вихідний сигнал, clipAmount(0..1) }.
    //
    // Математика: нижче (Threshold - kneeWidth) сигнал проходить без змін.
    // Вище цієї точки застосовується tanh-насичення, яке:
    //   * неперервне за значенням і похідною в точці kneeStart (C1-continuity),
    //   * асимптотично прямує до (kneeStart + kneeWidth) ≈ Threshold,
    //     тобто ніколи не "проколює" стелю, як справжній аналоговий clipper.
    // При knee01 -> 0 форма вироджується у звичайний hard-clip.
    static std::pair<float, float> process (float x, float thresholdLin, float knee01) noexcept
    {
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float absX = std::abs (x);

        if (knee01 < 0.001f)
        {
            // HARD CLIP: миттєве обрізання рівно по Threshold
            if (absX <= thresholdLin)
                return { x, 0.0f };

            return { sign * thresholdLin, 1.0f };
        }

        // Ширина knee-зони масштабується від softness. 1.0 (лінійна амплітуда)
        // обрано як максимум — цього достатньо, щоб при softness=100%
        // перехід був дуже пологим у всьому робочому діапазоні рівнів.
        const float kneeWidth = juce::jmax (0.0005f, knee01 * 1.0f);
        const float kneeStart = juce::jmax (0.0f, thresholdLin - kneeWidth);

        if (absX <= kneeStart)
            return { x, 0.0f };

        const float overshoot = absX - kneeStart;
        const float shaped = kneeWidth * std::tanh (overshoot / kneeWidth);
        const float outAbs = kneeStart + shaped;

        // Наскільки глибоко ми в knee-зоні (0..1) — визначає колір:
        // близько 0   -> щойно увійшли в soft-clip (помаранчевий, слабкий)
        // близько 1   -> практично на межі Threshold (майже hard, червоний)
        const float clipAmount = juce::jlimit (0.0f, 1.0f, shaped / kneeWidth);

        return { sign * outAbs, clipAmount };
    }
};

//==============================================================================
class ClipOnizerAudioProcessor : public juce::AudioProcessor
{
public:
    ClipOnizerAudioProcessor();
    ~ClipOnizerAudioProcessor() override;

    //==============================================================================
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

    // КРИТИЧНО ВАЖЛИВО: 0.0 — кліпер не додає latency (без lookahead/PDC).
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // ID параметрів (використовуються і в Processor, і в Editor)
    static constexpr const char* idInput     = "input";
    static constexpr const char* idOutput    = "output";
    static constexpr const char* idThreshold = "threshold";
    static constexpr const char* idSoftness  = "softness";
    static constexpr const char* idDelta     = "delta";

    //==============================================================================
    // ДАНІ ДЛЯ UI (thread-safe: пишуться в audio-thread, читаються з UI-таймера)

    // Кільцевий буфер для осцилографа.
    static constexpr int scopeBufferSize = 1 << 14; // 16384 семплів
    std::array<ScopeSample, scopeBufferSize> scopeBuffer {};
    std::atomic<int> scopeWritePos { 0 };

    // Поточний BPM хоста — для синхронізації часової шкали осцилографа.
    std::atomic<double> currentBpm { 120.0 };

    // Згладжена інтенсивність clipping для CLIP-лампи (0..1).
    std::atomic<float> clipIndicatorLevel { 0.0f };

    // Допоміжні методи для UI: щоб Editor міг намалювати transfer curve
    // (Clip Shaper), не дублюючи логіку конвертації параметрів.
    float getThresholdLinear() const noexcept;
    float getSoftnessNormalized() const noexcept;

private:
    juce::LinearSmoothedValue<float> inputGainSmoothed  { 1.0f };
    juce::LinearSmoothedValue<float> outputGainSmoothed { 1.0f };

    float clipEnvelope = 0.0f; // внутрішній стан envelope-follower для CLIP-лампи

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ClipOnizerAudioProcessor)
};
