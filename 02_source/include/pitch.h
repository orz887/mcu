#ifndef PITCH_H
#define PITCH_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 选学扩展：单音吉他调音器。
 * 初学时可先学习 GPIO、ADC、串口和游戏；本模块不影响这些基础功能。
 * 所有接口由主循环调用，不放在 ADC 或定时器中断里执行。
 */

/* PA0 音频每秒采集 8000 点；与主程序的 ADC 触发频率保持一致。 */
#define PITCH_SAMPLE_RATE 8000U
/* 每帧 1024 个原始 ADC 值，对应约 128 ms 音频。 */
#define PITCH_FRAME_SAMPLES 1024U
/* 比较波形相似程度时，每个延迟都固定比较 512 对采样点。 */
#define PITCH_WINDOW_SAMPLES 512U
/* 最多检查 128 个采样点的延迟，覆盖标准吉他的低音 E2。 */
#define PITCH_MAX_LAG 128U
/* 一次主循环最多计算两个延迟，把时间留给串口、按键和屏幕。 */
#define PITCH_MAX_STEP_LAGS 2U

typedef enum {
    PITCH_NO_SIGNAL,     /* 输入过弱，或还没有开始处理有效音频。 */
    PITCH_CLIPPED,       /* ADC 接近上下限，波形削顶，停止判断音高。 */
    PITCH_NO_PERIOD,     /* 找不到足够可靠的周期，常见原因是噪声。 */
    PITCH_OUT_OF_RANGE,  /* 检测到的频率不在 65～400 Hz 范围内。 */
    PITCH_VALID          /* 已得到通过置信度检查的音高结果。 */
} PitchStatus;

typedef struct {
    uint32_t frequency_millihz;     /* 频率乘 1000，例如 110000 表示 110 Hz。 */
    int16_t cents;                 /* 相对目标弦音的音分：正数偏高，负数偏低。 */
    uint16_t confidence_permille;  /* 0～1000 的周期相似度指标，并非统计概率。 */
    uint8_t string_index;          /* 0～5 依次为 E2、A2、D3、G3、B3、E4。 */
    bool valid;                   /* 仅为 true 时，频率、弦名和音分才可显示。 */
    PitchStatus status;           /* 无效原因或有效状态，供调试界面查看。 */
} PitchResult;

typedef struct {
    uint16_t samples[PITCH_FRAME_SAMPLES];  /* 本帧原始 12 位 ADC 值的独立副本。 */
    float normalized[PITCH_MAX_LAG + 1U];   /* 各延迟的归一化差分；越接近 0 越相似。 */
    uint64_t difference_sum;                /* 已计算延迟的差分总和，用于归一化。 */
    uint16_t next_lag;                      /* 下次 pitch_step 从这个延迟继续算。 */
    bool busy;                             /* true 表示正在计算，不能换入下一帧。 */
    PitchResult result;                    /* 最新结果；开始新帧时会清除旧结果。 */
} PitchDetector;

/* 上电或重新进入调音模式时初始化，清空进度和结果。 */
void pitch_init(PitchDetector *detector);

/*
 * 复制恰好 1024 个原始 ADC 值。成功接收返回 true；忙、空指针、长度不对返回 false。
 * 静音或削顶也算成功接收，但立即得到无效结果，不再计算周期。
 * 复制结束后调用方可以复用输入缓冲；detector 内部副本在计算完成前不会改变。
 */
bool pitch_begin(PitchDetector *detector, const uint16_t *samples, uint16_t count);

/*
 * 推进少量计算。lag_budget 为 0 时不推进，大于 2 时按 2 处理。
 * 返回 true 表示本帧已完成或当前空闲；false 表示还需在下次主循环继续调用。
 */
bool pitch_step(PitchDetector *detector, uint8_t lag_budget);

/* 查询是否正在计算；忙时不要尝试换入另一帧。 */
bool pitch_busy(const PitchDetector *detector);

/* 取得内部只读结果；先检查 valid，再使用频率、音分和弦序号。 */
const PitchResult *pitch_result(const PitchDetector *detector);

/* 把弦序号转换成屏幕使用的名称；无效序号返回 "--"。 */
const char *pitch_string_name(uint8_t string_index);

#endif
