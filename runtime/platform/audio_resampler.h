/*
 * audio_resampler.h - audio rate conversion, C port of the PcmMixer kernel
 * used by the Android audio path.
 *
 * A 16-tap Blackman-windowed sinc with 256 interpolated phases, precomputed
 * ONCE at stream open, never in the AAudio real-time callback.  Upsampling
 * (guest 22050/44100 Hz -> device 48000 Hz) is what suppresses the spectral
 * images that a two-point linear interpolator leaves in the output.
 *
 * Deliberately libm-free: sin/cos are evaluated with a short Taylor series
 * after range reduction, so neither the Android target nor the host T0 tests
 * need a new -lm link dependency.  Nothing here allocates or locks.
 */
#ifndef WC3_PLATFORM_AUDIO_RESAMPLER_H
#define WC3_PLATFORM_AUDIO_RESAMPLER_H

#include <stdint.h>

#define WC3_SINC_TAPS    16u
#define WC3_SINC_PHASES  256u

typedef struct Wc3SincKernel {
    float weights[WC3_SINC_PHASES + 1u][WC3_SINC_TAPS];
    int ready;
} Wc3SincKernel;

#define WC3_PI 3.14159265358979323846

/* sin(pi*x) for x in [-8, 8].  Reduce to u in [-0.5, 0.5] around the nearest
 * integer, keep the parity, and use the Taylor series of sin(pi*u). */
static inline double wc3_sin_pi(double x)
{
    double n = (x >= 0.0) ? (double)(long)(x + 0.5)
                          : (double)(long)(x - 0.5);
    double u = x - n;
    double y = WC3_PI * u;
    double y2 = y * y;
    double s = y * (1.0 - y2 / 6.0 + y2 * y2 / 120.0
                    - y2 * y2 * y2 / 5040.0
                    + y2 * y2 * y2 * y2 / 362880.0
                    - y2 * y2 * y2 * y2 * y2 / 39916800.0);
    return ((long)n & 1L) ? -s : s;
}

/* cos(pi*x) = sin(pi*(x + 0.5)). */
static inline double wc3_cos_pi(double x)
{
    return wc3_sin_pi(x + 0.5);
}

/* 10^(-attenuation), attenuation >= 0, evaluated without libm.  DirectSound
 * volume is in hundredths of a dB: gain = 10^(volume/2000). */
static inline float wc3_gain_from_attenuation(double attenuation)
{
    if (attenuation <= 0.0) return 1.0f;
    if (attenuation >= 12.0) return 0.0f;
    double t = attenuation * 2.30258509299404568402;
    long n = (long)t;
    double f = t - (double)n;
    /* exp(-f), f in [0,1): Taylor series. */
    double ef = 1.0 - f + f * f / 2.0 - f * f * f / 6.0
                + f * f * f * f / 24.0
                - f * f * f * f * f / 120.0
                + f * f * f * f * f * f / 720.0;
    double en = 1.0;
    for (long i = 0; i < n; i++) en *= 0.36787944117144232160;
    return (float)(en * ef);
}

static inline void wc3_sinc_init(Wc3SincKernel *k)
{
    for (unsigned p = 0; p <= WC3_SINC_PHASES; p++) {
        double sum = 0.0;
        for (unsigned t = 0; t < WC3_SINC_TAPS; t++) {
            double x = (double)(int)t - 7.0
                       - (double)p / (double)WC3_SINC_PHASES;
            double ax = x < 0.0 ? -x : x;
            double sinc = (ax < 1e-12) ? 1.0
                          : wc3_sin_pi(x) / (WC3_PI * x);
            double window = (ax > 8.0) ? 0.0
                            : 0.42 + 0.5 * wc3_cos_pi(x / 8.0)
                                   + 0.08 * wc3_cos_pi(x / 4.0);
            k->weights[p][t] = (float)(sinc * window);
            sum += k->weights[p][t];
        }
        if (sum == 0.0) sum = 1.0;
        for (unsigned t = 0; t < WC3_SINC_TAPS; t++)
            k->weights[p][t] = (float)((double)k->weights[p][t] / sum);
    }
    k->ready = 1;
}

#endif /* WC3_PLATFORM_AUDIO_RESAMPLER_H */
