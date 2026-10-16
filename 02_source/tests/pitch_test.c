#include "pitch.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 选学扩展的桌面回归测试：合成已知音高，再检查实际调音算法的输出。
 * 这些测试不用开发板，不能替代真实麦克风、环境噪声和吉他上的验证。
 * 测试中允许使用 double 合成信号；固件中的算法使用直观的 float 公式。
 */
static const double test_pi = 3.14159265358979323846;
static const double guitar_hz[] = {82.4069, 110.0, 146.8324, 195.9977, 246.9417, 329.6276};
static uint16_t frame[PITCH_FRAME_SAMPLES];
static PitchDetector detector;
static uint32_t noise_state = 1937U;

/* 固定种子的伪随机噪声使每次测试可以复现，不依赖系统随机数实现。 */
static int32_t random_noise(uint32_t amplitude)
{
    noise_state = noise_state * 1664525U + 1013904223U;
    return (int32_t)((noise_state >> 8U) % (2U * amplitude + 1U)) - (int32_t)amplitude;
}

/* 合成基频、二次/三次谐波、噪声与直流偏置，最后模拟 ADC 的 0～4095 范围。 */
static void make_tone(double frequency, double fundamental, double second,
                      double third, uint32_t noise, double bias)
{
    for (uint16_t sample = 0; sample < PITCH_FRAME_SAMPLES; ++sample) {
        double phase = 2.0 * test_pi * frequency * sample / PITCH_SAMPLE_RATE;
        double value = bias + fundamental * sin(phase + 0.37) +
                       second * sin(2.0 * phase + 0.7) +
                       third * sin(3.0 * phase + 1.1) + random_noise(noise);
        if (value < 0.0) {
            value = 0.0;
        } else if (value > 4095.0) {
            value = 4095.0;
        }
        frame[sample] = (uint16_t)lround(value);
    }
}

/* 按真实主循环的方式分步调用，检查完成标记和计算中结果不会提前变成有效。 */
static const PitchResult *run_frame(void)
{
    assert(pitch_begin(&detector, frame, PITCH_FRAME_SAMPLES));
    unsigned int steps = 0;
    while (pitch_busy(&detector)) {
        assert(!pitch_result(&detector)->valid);
        bool complete = pitch_step(&detector, 2U);
        assert(complete == !pitch_busy(&detector));
        assert(++steps <= PITCH_MAX_LAG);
    }
    return pitch_result(&detector);
}

/* 弦名必须正确，音分与频率误差都不得超过 8 音分，置信度至少为 820/1000。 */
static void expect_tone(unsigned int string_index, int expected_cents,
                        double fundamental, double second, double third,
                        uint32_t noise, double bias)
{
    double expected_hz = guitar_hz[string_index] * pow(2.0, expected_cents / 1200.0);
    make_tone(expected_hz, fundamental, second, third, noise, bias);
    const PitchResult *result = run_frame();
    double error_cents = result->frequency_millihz == 0U ? 10000.0 :
                        1200.0 * log2(result->frequency_millihz / (expected_hz * 1000.0));
    if (!result->valid || result->string_index != string_index ||
        abs(result->cents - expected_cents) > 8 || fabs(error_cents) > 8.0) {
        fprintf(stderr, "tone %u %+d: valid=%d status=%d note=%u hz=%.3f cents=%d error=%.2f confidence=%u\n",
                string_index, expected_cents, result->valid, result->status,
                result->string_index, result->frequency_millihz / 1000.0,
                result->cents, error_cents, result->confidence_permille);
        for (uint16_t lag = 2U; lag < PITCH_MAX_LAG; ++lag) {
            if (detector.normalized[lag] < detector.normalized[lag - 1U] &&
                detector.normalized[lag] < detector.normalized[lag + 1U]) {
                fprintf(stderr, "lag=%u curve=%.6f,%.6f,%.6f\n", lag,
                        detector.normalized[lag - 1U], detector.normalized[lag],
                        detector.normalized[lag + 1U]);
            }
        }
        abort();
    }
    assert(result->status == PITCH_VALID);
    assert(result->confidence_permille >= 820U);
}

/* 六根标准弦分别检查准确音高、偏低 40 音分、偏高 40 音分和不同直流偏置。 */
static void test_standard_strings_and_detuning(void)
{
    for (unsigned int string_index = 0; string_index < 6U; ++string_index) {
        expect_tone(string_index, 0, 700.0, 0.0, 0.0, 0U, 1800.0);
        expect_tone(string_index, -40, 650.0, 0.0, 0.0, 35U, 2200.0);
        expect_tone(string_index, 40, 650.0, 0.0, 0.0, 35U, 1700.0);
    }
}

/* 二次谐波远强于基频时，仍须找回基频，不能误显示高八度。 */
static void test_harmonics_do_not_report_octave(void)
{
    for (unsigned int string_index = 0; string_index < 6U; ++string_index) {
        expect_tone(string_index, 0, 180.0, 850.0, 200.0, 20U, 2048.0);
        expect_tone(string_index, -25, 120.0, 800.0, 0.0, 15U, 2048.0);
    }
}

/* 在每根弦前后 80 音分范围内每隔 10 音分检查一次，覆盖非整数采样周期。 */
static void test_frequency_sweep(void)
{
    for (unsigned int string_index = 0; string_index < 6U; ++string_index) {
        for (int cents = -80; cents <= 80; cents += 10) {
            expect_tone(string_index, cents, 150.0, 700.0, 80.0, 25U, 2048.0);
        }
    }
}

/* 给音量乘以指数衰减，模拟拨弦后声音逐渐变小，不要求振幅绝对恒定。 */
static void test_decaying_plucked_string(void)
{
    for (unsigned int string_index = 0; string_index < 6U; ++string_index) {
        make_tone(guitar_hz[string_index], 400.0, 650.0, 200.0, 0U, 2048.0);
        for (uint16_t sample = 0U; sample < PITCH_FRAME_SAMPLES; ++sample) {
            double envelope = exp(-(double)sample / PITCH_FRAME_SAMPLES);
            frame[sample] = (uint16_t)lround(2048.0 + ((double)frame[sample] - 2048.0) * envelope);
        }
        const PitchResult *result = run_frame();
        assert(result->valid);
        assert(result->string_index == string_index);
        assert(abs(result->cents) <= 8);
    }
}

/* 逐步增加噪声；允许算法拒绝结果，但任何有效结果都必须守住置信度下限。 */
static void test_confidence_floor(void)
{
    for (unsigned int string_index = 0U; string_index < 6U; ++string_index) {
        for (uint32_t noise = 0U; noise <= 500U; noise += 5U) {
            make_tone(guitar_hz[string_index], 200.0, 0.0, 0.0, noise, 2048.0);
            const PitchResult *result = run_frame();
            assert(!result->valid || result->confidence_permille >= 820U);
        }
    }
}

/* 静音、弱信号、削顶、随机噪声、超出检测范围和非法 ADC 值均不得显示有效音高。 */
static void test_invalid_audio(void)
{
    for (uint16_t sample = 0; sample < PITCH_FRAME_SAMPLES; ++sample) {
        frame[sample] = 2048U;
    }
    assert(!run_frame()->valid);
    assert(pitch_result(&detector)->status == PITCH_NO_SIGNAL);
    make_tone(110.0, 12.0, 0.0, 0.0, 1U, 2048.0);
    assert(!run_frame()->valid);
    assert(pitch_result(&detector)->status == PITCH_NO_SIGNAL);
    make_tone(110.0, 3000.0, 0.0, 0.0, 0U, 2048.0);
    assert(!run_frame()->valid);
    assert(pitch_result(&detector)->status == PITCH_CLIPPED);
    for (unsigned int trial = 0; trial < 16U; ++trial) {
        make_tone(110.0, 0.0, 0.0, 0.0, 700U, 2048.0);
        assert(!run_frame()->valid);
        assert(pitch_result(&detector)->status == PITCH_NO_PERIOD);
    }
    make_tone(110.0, 75.0, 0.0, 0.0, 700U, 2048.0);
    assert(!run_frame()->valid);
    make_tone(45.0, 700.0, 0.0, 0.0, 0U, 2048.0);
    assert(!run_frame()->valid);
    make_tone(500.0, 700.0, 0.0, 0.0, 0U, 2048.0);
    assert(!run_frame()->valid);
    assert(pitch_result(&detector)->status == PITCH_OUT_OF_RANGE);
    frame[0] = UINT16_MAX;
    assert(!run_frame()->valid);
    assert(pitch_result(&detector)->status == PITCH_CLIPPED);
}

/* 检查每步预算、忙时拒绝换帧、原始 ADC 完整保留，以及外部缓冲复用的安全性。 */
static void test_cooperative_budget_and_owned_frame(void)
{
    pitch_init(&detector);
    assert(!pitch_busy(&detector));
    assert(pitch_step(&detector, 2U));
    assert(!pitch_begin(&detector, frame, PITCH_FRAME_SAMPLES - 1U));
    assert(!pitch_begin(&detector, NULL, PITCH_FRAME_SAMPLES));
    make_tone(110.0, 700.0, 0.0, 0.0, 0U, 2048.0);
    assert(pitch_begin(&detector, frame, PITCH_FRAME_SAMPLES));
    assert(pitch_busy(&detector));
    /* 原始 ADC 数字应保持原样，便于初学者在调试器中对照采样结果。 */
    assert(memcmp(detector.samples, frame, sizeof(frame)) == 0);
    uint16_t initial_lag = detector.next_lag;
    assert(!pitch_step(&detector, 0U));
    assert(detector.next_lag == initial_lag);
    assert(!pitch_step(&detector, 1U));
    assert(detector.next_lag == initial_lag + 1U);
    assert(!pitch_step(&detector, 255U));
    assert(detector.next_lag == initial_lag + 3U);
    assert(!pitch_begin(&detector, frame, PITCH_FRAME_SAMPLES));
    memset(frame, 0, sizeof(frame));
    while (!pitch_step(&detector, 2U)) {
    }
    assert(pitch_result(&detector)->valid);
    assert(pitch_result(&detector)->string_index == 1U);
    assert(abs(pitch_result(&detector)->cents) <= 8);
    assert(strcmp(pitch_string_name(0U), "E2") == 0);
    assert(strcmp(pitch_string_name(5U), "E4") == 0);
    assert(strcmp(pitch_string_name(6U), "--") == 0);
}

/* 顺序运行全部行为测试；任何一项不满足要求都会立即报错退出。 */
int main(void)
{
    assert(sizeof(PitchDetector) < 3072U);
    pitch_init(&detector);
    test_standard_strings_and_detuning();
    test_harmonics_do_not_report_octave();
    test_frequency_sweep();
    test_decaying_plucked_string();
    test_confidence_floor();
    test_invalid_audio();
    test_cooperative_budget_and_owned_frame();
    printf("pitch tests passed; detector=%zu bytes\n", sizeof(PitchDetector));
    return 0;
}
