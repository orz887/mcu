#include "pitch.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

/*
 * 选学扩展：用波形的重复周期判断吉他单音。
 * 相隔一个周期的两个采样点应当很接近，把差值平方后累加，就能找到周期。
 * 这里采用 YIN 的累计均值归一化思路，减轻短延迟和振幅变化造成的干扰。
 * 为便于阅读，归一化、插值和音分公式直接使用 float，不使用定点位运算。
 * STM32F103 没有硬件浮点单元，因此重活仍分成多次 pitch_step 执行。
 */

#define PITCH_PERIOD_THRESHOLD 0.18f
#define PITCH_MIN_FREQUENCY_HZ 65.0f
#define PITCH_MAX_FREQUENCY_HZ 400.0f
#define PITCH_MIN_PEAK_TO_PEAK 96U
#define PITCH_MIN_RMS 24U

/* 标准调弦的六个基频，顺序由最低的 E2 到最高的 E4。 */
static const float standard_hz[] = {
    82.4069f, 110.0f, 146.8324f, 195.9977f, 246.9417f, 329.6276f
};

/*
 * 用谷底及左右邻点拟合抛物线，估计谷底在小数位置上的高度。
 * 真实周期通常不是整数采样点：只比较整数谷底，高音弦容易误选倍周期。
 */
static float minimum_value(const PitchDetector *detector, uint16_t lag)
{
    float left = detector->normalized[lag - 1U];
    float center = detector->normalized[lag];
    float right = detector->normalized[lag + 1U];
    float curvature = left - 2.0f * center + right;
    if (curvature <= 0.0f) {
        return center;
    }
    float slope = left - right;
    float interpolated = center - slope * slope / (8.0f * curvature);
    return interpolated > 0.0f ? interpolated : 0.0f;
}

/* 音分定义：频率翻倍是 1200 音分；正值表示比参考音高，负值表示偏低。 */
static float frequency_cents(float frequency_hz, float reference_hz)
{
    return 1200.0f * log2f(frequency_hz / reference_hz);
}

/*
 * 所有延迟计算完后，从差分曲线找周期，再换算成频率、最近的标准弦音和音分。
 * 这里只扫描约 128 个结果，不再逐点比较音频，因此不会重复执行整帧重计算。
 */
static void finish_frame(PitchDetector *detector)
{
    float best_value = FLT_MAX;
    for (uint16_t lag = 2U; lag < PITCH_MAX_LAG; ++lag) {
        float current = detector->normalized[lag];
        if (current <= detector->normalized[lag - 1U] &&
            current < detector->normalized[lag + 1U]) {
            float interpolated = minimum_value(detector, lag);
            if (interpolated < best_value) {
                best_value = interpolated;
            }
        }
    }
    detector->busy = false;
    detector->result.status = PITCH_NO_PERIOD;
    if (best_value > PITCH_PERIOD_THRESHOLD) {
        return;
    }

    /*
     * 优先选接近最深谷底的第一个周期，而不是直接选第一个较浅的谷。
     * 吉他的二次谐波有时比基频更强，这一步可减少误报高八度。
     * 同时允许约 0.011 的插值误差，避免把两倍、三倍周期误当基周期。
     */
    float allowance = best_value * 0.5f;
    if (allowance < 0.011f) {
        allowance = 0.011f;
    }
    float candidate_limit = best_value + allowance;
    if (candidate_limit > PITCH_PERIOD_THRESHOLD) {
        candidate_limit = PITCH_PERIOD_THRESHOLD;
    }
    uint16_t chosen_lag = 0U;
    for (uint16_t lag = 2U; lag < PITCH_MAX_LAG; ++lag) {
        float current = detector->normalized[lag];
        if (current <= detector->normalized[lag - 1U] &&
            current < detector->normalized[lag + 1U] &&
            minimum_value(detector, lag) <= candidate_limit) {
            chosen_lag = lag;
            break;
        }
    }
    if (chosen_lag == 0U) {
        return;
    }
    float left = detector->normalized[chosen_lag - 1U];
    float center = detector->normalized[chosen_lag];
    float right = detector->normalized[chosen_lag + 1U];

    /* 置信度以真实采样点为准；插值更深，不能因此放行低置信度噪声。 */
    if (center > PITCH_PERIOD_THRESHOLD) {
        return;
    }

    /* 抛物线顶点最多偏离所选整数延迟半个采样点。 */
    float curvature = left - 2.0f * center + right;
    float offset = 0.0f;
    if (curvature > 0.0f) {
        offset = 0.5f * (left - right) / curvature;
        if (offset > 0.5f) {
            offset = 0.5f;
        } else if (offset < -0.5f) {
            offset = -0.5f;
        }
    }
    float period_samples = chosen_lag + offset;
    float frequency_hz = PITCH_SAMPLE_RATE / period_samples;
    if (frequency_hz < PITCH_MIN_FREQUENCY_HZ || frequency_hz > PITCH_MAX_FREQUENCY_HZ) {
        detector->result.status = PITCH_OUT_OF_RANGE;
        return;
    }

    /* 音高距离按音分比较；这比直接比较相差多少 Hz 更符合音乐上的远近。 */
    uint8_t nearest_string = 0U;
    float nearest_cents = frequency_cents(frequency_hz, standard_hz[0]);
    for (uint8_t string_index = 1U; string_index < 6U; ++string_index) {
        float cents = frequency_cents(frequency_hz, standard_hz[string_index]);
        if (fabsf(cents) < fabsf(nearest_cents)) {
            nearest_string = string_index;
            nearest_cents = cents;
        }
    }
    detector->result.frequency_millihz = (uint32_t)lroundf(frequency_hz * 1000.0f);
    detector->result.cents = (int16_t)lroundf(nearest_cents);
    detector->result.confidence_permille = (uint16_t)lroundf((1.0f - center) * 1000.0f);
    detector->result.string_index = nearest_string;
    detector->result.status = PITCH_VALID;
    detector->result.valid = true;
}

/* 清空输入缓存、进度和旧结果；初始化后处于空闲状态。 */
void pitch_init(PitchDetector *detector)
{
    memset(detector, 0, sizeof(*detector));
    detector->result.status = PITCH_NO_SIGNAL;
}

/*
 * 接收完整音频帧并做便宜的预检查：静音与削顶不必进入周期计算。
 * 保留原始 ADC 数值，不压缩位数，方便在调试器中直接对照 ADC 数据。
 */
bool pitch_begin(PitchDetector *detector, const uint16_t *samples, uint16_t count)
{
    if (detector->busy || samples == NULL || count != PITCH_FRAME_SAMPLES) {
        return false;
    }
    memset(&detector->result, 0, sizeof(detector->result));
    detector->result.status = PITCH_NO_SIGNAL;
    uint16_t minimum = UINT16_MAX;
    uint16_t maximum = 0U;
    uint64_t sample_sum = 0U;
    uint64_t square_sum = 0U;
    bool clipped = false;
    for (uint16_t sample = 0U; sample < count; ++sample) {
        uint16_t raw = samples[sample];
        if (raw <= 5U || raw >= 4090U) {
            clipped = true;
        }
        detector->samples[sample] = raw;
        sample_sum += raw;
        square_sum += (uint64_t)raw * raw;
        if (raw < minimum) {
            minimum = raw;
        }
        if (raw > maximum) {
            maximum = raw;
        }
    }
    if (clipped) {
        detector->result.status = PITCH_CLIPPED;
        return true;
    }

    /*
     * 去直流后的均方值 = 平方和 / 点数 - 平均值的平方。
     * 先把等式两边乘以点数的平方，可完全用整数判断 RMS 是否达到 24。
     * 12 位 ADC 的平方和会超过 32 位，使用 uint64_t 防止溢出。
     */
    uint64_t centered_energy = square_sum * count - sample_sum * sample_sum;
    uint64_t minimum_energy = (uint64_t)PITCH_MIN_RMS * PITCH_MIN_RMS * count * count;
    if ((uint16_t)(maximum - minimum) < PITCH_MIN_PEAK_TO_PEAK || centered_energy < minimum_energy) {
        return true;
    }
    detector->difference_sum = 0U;
    detector->normalized[0] = 1.0f;
    detector->next_lag = 1U;
    detector->busy = true;
    return true;
}

/*
 * 每次只计算一两个延迟。主循环反复调用，就能计算完整曲线而不长时间卡住串口。
 * 512 点窗口和最大 128 点延迟只用到样本 0～639；整帧 1024 点均参与预检查。
 */
bool pitch_step(PitchDetector *detector, uint8_t lag_budget)
{
    if (!detector->busy) {
        return true;
    }
    if (lag_budget > PITCH_MAX_STEP_LAGS) {
        lag_budget = PITCH_MAX_STEP_LAGS;
    }
    for (uint8_t computed = 0U; computed < lag_budget; ++computed) {
        uint16_t lag = detector->next_lag;
        uint64_t difference = 0U;
        for (uint16_t sample = 0U; sample < PITCH_WINDOW_SAMPLES; ++sample) {
            int32_t delta = (int32_t)detector->samples[sample] - detector->samples[sample + lag];
            /* 单个差值平方适合 int32_t，512 项累加必须使用 uint64_t。 */
            difference += (uint64_t)(delta * delta);
        }

        /* 当前差分 / 从延迟 1 到当前延迟的平均差分，即 YIN 归一化差分。 */
        detector->difference_sum += difference;
        if (detector->difference_sum == 0U) {
            detector->normalized[lag] = 1.0f;
        } else {
            detector->normalized[lag] = (float)difference * lag / (float)detector->difference_sum;
        }
        ++detector->next_lag;
        if (detector->next_lag > PITCH_MAX_LAG) {
            finish_frame(detector);
            return true;
        }
    }
    return false;
}

/* 主循环通过此函数决定继续计算还是接收下一帧。 */
bool pitch_busy(const PitchDetector *detector)
{
    return detector->busy;
}

/* 返回内部结果地址，不产生额外副本；调用方只能读取。 */
const PitchResult *pitch_result(const PitchDetector *detector)
{
    return &detector->result;
}

/* 返回标准弦音名称，供屏幕和串口显示使用。 */
const char *pitch_string_name(uint8_t string_index)
{
    static const char *const names[] = {"E2", "A2", "D3", "G3", "B3", "E4"};
    return string_index < 6U ? names[string_index] : "--";
}
