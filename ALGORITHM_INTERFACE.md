# BPM 与 HRV 模块接口

五个模块只依赖标准 C11，不依赖 HAL。采集模块把每个原始 ADC 样本交给 `ecg_preprocess`，`qrs_detector` 从检测通道产生 R 峰采样序号，`heart_rate` 和 `hrv` 计算结果；可选的 `sample_rate_estimator` 用独立毫秒时钟校准实际采样率。显示、蜂鸣器、LED 和无线模块只需读取结果或订阅报警回调。

## 通用预处理

```c
#include "ecg_preprocess.h"

static ECGPreprocessContext preprocess;

void Preprocess_Init(void)
{
    ECGPreprocess_Init(&preprocess, NULL); /* 默认按 500 Hz 系数 */
}

void OnAdcSample(uint16_t raw_adc, uint32_t sample_index)
{
    const ECGPreprocessResult *sample =
        ECGPreprocess_Push(&preprocess, (float)raw_adc);

    /* sample->display_sample 交给 TFT 波形；
     * sample->qrs_sample 交给 QRS 检测；
     * ready_for_qrs=false 时保留数据，但暂停产生新的 R 峰事件。 */
    (void)sample_index;
}
```

默认预处理包含 0.5--40 Hz 显示通道、5--15 Hz QRS 通道、0.5 s 削顶/平直窗口、突变与输入范围标志，以及启动后 2 s 学习期。所有样本都会进入滤波器，质量标志只用于门控，不删除原始数据。滤波系数按 500 Hz 设计；约 475--525 Hz 的常见误差可以直接使用，偏差更大时应重新生成系数。

## 原始样本到 R 峰

```c
#include "qrs_detector.h"

static QRSDetectorContext qrs_detector;

void QRS_Init(void)
{
    QRSDetector_Init(&qrs_detector, NULL); /* 默认 500 Hz */
}

void OnFilteredSample(uint32_t sample_index, float qrs_sample)
{
    uint32_t r_sample_index;
    if (QRSDetector_Push(&qrs_detector,
                         sample_index,
                         qrs_sample,
                         &r_sample_index))
    {
        OnRPeakDetected(r_sample_index);
    }
}
```

检测器使用能量包络、信号/噪声双估计、动态阈值和 250 ms 不应期。运行时不保存波形历史，内存占用为常数。

## 初始化

```c
#include "heart_rate.h"
#include "hrv.h"

static HeartRateContext heart_rate;
static HRVContext hrv;

void Algorithm_Init(void)
{
    const HeartRateConfig heart_rate_config = {
        .sample_rate_hz = 500U,
        .min_bpm = 40U,
        .max_bpm = 180U,
    };

    HeartRate_Init(&heart_rate, &heart_rate_config);
    HRV_Init(&hrv, NULL); /* 使用预设计报告的报警阈值 */
}
```

如果采集端可以提供独立的毫秒时钟，可用常数内存估计实际采样率：

```c
#include "sample_rate_estimator.h"

static SampleRateEstimator rate_estimator;

void SampleClock_Init(void)
{
    SampleRateEstimator_Init(&rate_estimator, NULL);
}

void OnSampleClock(uint32_t sample_index, uint32_t elapsed_ms)
{
    if (SampleRateEstimator_Push(&rate_estimator,
                                 sample_index,
                                 elapsed_ms))
    {
        uint32_t rate_millihz =
            SampleRateEstimator_GetMilliHz(&rate_estimator);
        HeartRate_SetSampleRateMilliHz(&heart_rate, rate_millihz);
        QRSDetector_SetSampleRateMilliHz(&qrs_detector, rate_millihz);
    }
}
```

估计器默认至少观察 5 秒，只保存起始采样号、起始时间和当前估计值，不保存采样数组；时间源不可用或估计超出 450--550 Hz 时自动保持 500 Hz。

如需在报警状态切换时直接驱动声光模块，可传入回调：

```c
static void RhythmAlarmChanged(bool active, void *user_data)
{
    (void)user_data;
    /* active=true: 启动 LED/蜂鸣器并置无线报警位；false: 解除。 */
}

static const HRVConfig hrv_config = {
    .alarm_cv_threshold = 0.10F,
    .alarm_d_threshold = 0.12F,
    .clear_cv_threshold = 0.08F,
    .clear_d_threshold = 0.10F,
    .alarm_confirm_updates = 3U,
    .clear_confirm_updates = 5U,
    .alarm_callback = RhythmAlarmChanged,
    .callback_user_data = NULL,
};
```

## R 峰输入

QRS 检测模块每确认一个 R 峰后调用：

```c
void OnRPeakDetected(uint32_t r_sample_index)
{
    HeartRateEvent event = HeartRate_PushRPeak(&heart_rate, r_sample_index);

    if (event == HEART_RATE_EVENT_UPDATED)
    {
        const HeartRateResult *rate = HeartRate_GetResult(&heart_rate);
        HRVAlarmEvent alarm_event = HRV_PushRR(&hrv, rate->rr_ms);

        /* rate->display_bpm: 最近最多 5 个 RR 的中位数心率
         * rate->instantaneous_bpm: 本次瞬时心率
         * rate->stable: 已积满 5 个 RR，可用于模拟器稳定值验收
         * alarm_event: 本次是否触发或解除报警 */
        (void)alarm_event;
    }
}
```

`r_sample_index` 必须与 ADC 的连续采样序号使用同一时基。模块支持 `uint32_t` 自然回绕。40...180 BPM 外的 RR 会被拒绝，且不会污染最近 5 个 RR 窗口。

## 信号质量门控

导联脱落、削顶、ADC 丢块、明显伪迹或 QRS 检测不稳时调用：

```c
void OnSignalInvalid(void)
{
    HeartRate_Invalidate(&heart_rate);
    HRV_Invalidate(&hrv);
}

void OnSignalRecovered(void)
{
    HeartRate_Invalidate(&heart_rate); /* 下一个 R 峰只建立新锚点 */
    HRV_SetSignalValid(&hrv);          /* 重新累计连续 30 个 RR */
}
```

这样不会用故障前后的两个 R 峰组成跨缺口 RR，也会在无效期间抑制节律报警。

## 显示与无线输出

```c
const HeartRateResult *rate = HeartRate_GetResult(&heart_rate);
const HRVResult *variability = HRV_GetResult(&hrv);
```

- `rate->valid`：BPM 是否可显示；无效时显示 `--`。
- `rate->display_bpm`：实时 BPM；`stable=true` 表示 5 个 RR 已填满。
- `variability->status`：数据不足、信号无效、正常或疑似节律异常。
- `variability->rr_count`：未满 30 个 RR 时可显示积累进度。
- `variability->sdnn_ms`、`rmssd_ms`、`cv`、`d`：短窗统计量。
- `variability->alarm_active`：持续报警状态，适合放入每个无线状态帧。
- `variability->alarm_event`：仅表示最近一次更新的触发/解除边沿。

该 HRV 判据用于课程实验中的节律变化提示，不作临床诊断。
