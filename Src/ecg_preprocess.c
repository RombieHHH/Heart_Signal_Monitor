#include "ecg_preprocess.h"

#include <stddef.h>
#include <string.h>

#define DEFAULT_SAMPLE_RATE_HZ 500U
#define DEFAULT_QUALITY_WINDOW_SAMPLES 250U
#define DEFAULT_LEARNING_SAMPLES 1000U

static const ECGBiquadCoefficients DISPLAY_HIGHPASS_0P5_HZ = {
    0.99556697F, -1.99113394F, 0.99556697F, -1.99111429F, 0.99115360F};
static const ECGBiquadCoefficients DISPLAY_LOWPASS_40_HZ = {
    0.04613180F, 0.09226360F, 0.04613180F, -1.30728503F, 0.49181224F};
static const ECGBiquadCoefficients QRS_HIGHPASS_5_HZ = {
    0.95654323F, -1.91308645F, 0.95654323F, -1.91119707F, 0.91497583F};
static const ECGBiquadCoefficients QRS_LOWPASS_15_HZ = {
    0.00782021F, 0.01564042F, 0.00782021F, -1.73472577F, 0.76600660F};

static float AbsFloat(float value)
{
    return (value < 0.0F) ? -value : value;
}

static void BiquadSetSteadyState(ECGBiquad *filter,
                                 float input,
                                 float desired_output)
{
    filter->state1 = desired_output - filter->coefficients.b0 * input;
    filter->state2 = filter->coefficients.b2 * input -
                     filter->coefficients.a2 * desired_output;
}

static float BiquadPush(ECGBiquad *filter, float input)
{
    float output = filter->coefficients.b0 * input + filter->state1;
    filter->state1 = filter->coefficients.b1 * input -
                     filter->coefficients.a1 * output + filter->state2;
    filter->state2 = filter->coefficients.b2 * input -
                     filter->coefficients.a2 * output;
    return output;
}

static bool IsClipped(const ECGPreprocessContext *context, float sample)
{
    return (sample <= context->config.clip_low) ||
           (sample >= context->config.clip_high);
}

static void UpdateQualityWindow(ECGPreprocessContext *context, float sample)
{
    uint16_t slot = context->quality_write_index;

    if (context->quality_count == context->config.quality_window_samples)
    {
        if (IsClipped(context, context->quality_window[slot]) &&
            (context->clipped_count > 0U))
        {
            --context->clipped_count;
        }
    }
    else
    {
        ++context->quality_count;
    }

    context->quality_window[slot] = sample;
    if (IsClipped(context, sample))
    {
        ++context->clipped_count;
    }
    context->quality_write_index =
        (uint16_t)((slot + 1U) % context->config.quality_window_samples);
}

static void CalculateWindowQuality(ECGPreprocessContext *context)
{
    float minimum = context->quality_window[0];
    float maximum = context->quality_window[0];
    uint16_t i;

    for (i = 1U; i < context->quality_count; ++i)
    {
        float value = context->quality_window[i];
        if (value < minimum)
        {
            minimum = value;
        }
        if (value > maximum)
        {
            maximum = value;
        }
    }

    context->result.window_peak_to_peak = maximum - minimum;
    context->result.clipped_fraction =
        (float)context->clipped_count / (float)context->quality_count;

    if (context->quality_count == context->config.quality_window_samples)
    {
        if (context->result.clipped_fraction > context->config.clip_fraction)
        {
            context->result.quality_flags |= ECG_QUALITY_CLIPPED;
        }
        if (context->result.window_peak_to_peak <
            context->config.flatline_peak_to_peak)
        {
            context->result.quality_flags |= ECG_QUALITY_FLATLINE;
        }
    }
}

void ECGPreprocess_DefaultConfig(ECGPreprocessConfig *config)
{
    if (config == NULL)
    {
        return;
    }

    config->sample_rate_hz = DEFAULT_SAMPLE_RATE_HZ;
    config->quality_window_samples = DEFAULT_QUALITY_WINDOW_SAMPLES;
    config->learning_samples = DEFAULT_LEARNING_SAMPLES;
    config->adc_min = 0.0F;
    config->adc_max = 4095.0F;
    config->clip_low = 10.0F;
    config->clip_high = 4085.0F;
    config->clip_fraction = 0.05F;
    config->flatline_peak_to_peak = 12.0F;
    config->step_threshold = 800.0F;
}

void ECGPreprocess_Init(ECGPreprocessContext *context,
                        const ECGPreprocessConfig *config)
{
    ECGPreprocessConfig defaults;

    if (context == NULL)
    {
        return;
    }

    ECGPreprocess_DefaultConfig(&defaults);
    memset(context, 0, sizeof(*context));
    context->config = (config != NULL) ? *config : defaults;

    if ((context->config.quality_window_samples == 0U) ||
        (context->config.quality_window_samples >
         ECG_PREPROCESS_QUALITY_WINDOW_MAX))
    {
        context->config.quality_window_samples =
            defaults.quality_window_samples;
    }
    if (context->config.learning_samples == 0U)
    {
        context->config.learning_samples = defaults.learning_samples;
    }

    context->display_highpass.coefficients = DISPLAY_HIGHPASS_0P5_HZ;
    context->display_lowpass.coefficients = DISPLAY_LOWPASS_40_HZ;
    context->qrs_highpass.coefficients = QRS_HIGHPASS_5_HZ;
    context->qrs_lowpass.coefficients = QRS_LOWPASS_15_HZ;
}

void ECGPreprocess_Reset(ECGPreprocessContext *context, float initial_raw)
{
    if (context == NULL)
    {
        return;
    }

    memset(context->quality_window, 0, sizeof(context->quality_window));
    context->result.raw_sample = initial_raw;
    context->result.display_sample = 0.0F;
    context->result.qrs_sample = 0.0F;
    context->result.window_peak_to_peak = 0.0F;
    context->result.clipped_fraction = 0.0F;
    context->result.quality_flags = ECG_QUALITY_WARMING_UP;
    context->result.ready_for_qrs = false;
    context->previous_raw = initial_raw;
    context->processed_samples = 0U;
    context->quality_write_index = 0U;
    context->quality_count = 0U;
    context->clipped_count = 0U;
    context->initialized = true;

    BiquadSetSteadyState(&context->display_highpass, initial_raw, 0.0F);
    BiquadSetSteadyState(&context->display_lowpass, 0.0F, 0.0F);
    BiquadSetSteadyState(&context->qrs_highpass, initial_raw, 0.0F);
    BiquadSetSteadyState(&context->qrs_lowpass, 0.0F, 0.0F);
}

const ECGPreprocessResult *ECGPreprocess_Push(ECGPreprocessContext *context,
                                              float raw_sample)
{
    float display_highpassed;
    float qrs_highpassed;

    if (context == NULL)
    {
        return NULL;
    }
    if (!context->initialized)
    {
        ECGPreprocess_Reset(context, raw_sample);
    }

    context->result.raw_sample = raw_sample;
    context->result.quality_flags = ECG_QUALITY_OK;
    display_highpassed = BiquadPush(&context->display_highpass, raw_sample);
    context->result.display_sample =
        BiquadPush(&context->display_lowpass, display_highpassed);
    qrs_highpassed = BiquadPush(&context->qrs_highpass, raw_sample);
    context->result.qrs_sample =
        BiquadPush(&context->qrs_lowpass, qrs_highpassed);

    UpdateQualityWindow(context, raw_sample);
    CalculateWindowQuality(context);

    if ((raw_sample < context->config.adc_min) ||
        (raw_sample > context->config.adc_max))
    {
        context->result.quality_flags |= ECG_QUALITY_OUT_OF_RANGE;
    }
    if ((context->processed_samples > 0U) &&
        (AbsFloat(raw_sample - context->previous_raw) >
         context->config.step_threshold))
    {
        context->result.quality_flags |= ECG_QUALITY_STEP_ARTIFACT;
    }
    if (context->processed_samples < context->config.learning_samples)
    {
        context->result.quality_flags |= ECG_QUALITY_WARMING_UP;
    }

    context->previous_raw = raw_sample;
    if (context->processed_samples < UINT32_MAX)
    {
        ++context->processed_samples;
    }
    context->result.ready_for_qrs =
        (context->result.quality_flags == ECG_QUALITY_OK);
    return &context->result;
}
