#ifndef RADIOIFY_VIDEO_COLOR_MATH_HLSLI
#define RADIOIFY_VIDEO_COLOR_MATH_HLSLI

static const float RADIOIFY_PQ_REFERENCE_MAX_NITS = 10000.0;

float RadioifyBt1886ToLinear(float x)
{
    return pow(saturate(x), 2.4);
}

float3 RadioifyBt1886ToLinear(float3 x)
{
    return float3(RadioifyBt1886ToLinear(x.r),
                  RadioifyBt1886ToLinear(x.g),
                  RadioifyBt1886ToLinear(x.b));
}

float RadioifySrgbToLinear(float v)
{
    v = saturate(v);
    return (v <= 0.04045) ? (v / 12.92)
                          : pow((v + 0.055) / 1.055, 2.4);
}

float3 RadioifySrgbToLinear(float3 v)
{
    return float3(RadioifySrgbToLinear(v.r),
                  RadioifySrgbToLinear(v.g),
                  RadioifySrgbToLinear(v.b));
}

float RadioifyLinearToSrgb(float v)
{
    v = saturate(v);
    return (v <= 0.0031308) ? (v * 12.92)
                            : (1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

float3 RadioifyLinearToSrgb(float3 v)
{
    return float3(RadioifyLinearToSrgb(v.r),
                  RadioifyLinearToSrgb(v.g),
                  RadioifyLinearToSrgb(v.b));
}

float3 RadioifyLinearBt709ToBt2020(float3 v)
{
    return float3(
        0.6274 * v.r + 0.3293 * v.g + 0.0433 * v.b,
        0.0691 * v.r + 0.9195 * v.g + 0.0114 * v.b,
        0.0164 * v.r + 0.0880 * v.g + 0.8956 * v.b);
}

float3 RadioifyLinearBt2020ToBt709(float3 v)
{
    return float3(
        1.6605 * v.r - 0.5876 * v.g - 0.0728 * v.b,
       -0.1246 * v.r + 1.1329 * v.g - 0.0083 * v.b,
       -0.0182 * v.r - 0.1006 * v.g + 1.1187 * v.b);
}

float RadioifyPqEotf(float v)
{
    float m1 = 2610.0 / 16384.0;
    float m2 = 2523.0 / 32.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 128.0;
    float c3 = 2392.0 / 128.0;
    float vp = pow(max(v, 0.0), 1.0 / m2);
    float denom = max(c2 - c3 * vp, 1.0e-6);
    return pow(max((vp - c1) / denom, 0.0), 1.0 / m1);
}

float3 RadioifyPqEotf(float3 v)
{
    return float3(RadioifyPqEotf(v.r),
                  RadioifyPqEotf(v.g),
                  RadioifyPqEotf(v.b));
}

float RadioifyPqOetf(float v)
{
    float m1 = 2610.0 / 16384.0;
    float m2 = 2523.0 / 32.0;
    float c1 = 3424.0 / 4096.0;
    float c2 = 2413.0 / 128.0;
    float c3 = 2392.0 / 128.0;
    float vp = pow(max(v, 0.0), m1);
    return pow((c1 + c2 * vp) / (1.0 + c3 * vp), m2);
}

float3 RadioifyPqOetfFromNits(float3 nits)
{
    float3 relative = max(nits, float3(0.0, 0.0, 0.0)) /
                      RADIOIFY_PQ_REFERENCE_MAX_NITS;
    return float3(RadioifyPqOetf(relative.r),
                  RadioifyPqOetf(relative.g),
                  RadioifyPqOetf(relative.b));
}

float RadioifyHlgEotf(float v)
{
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    if (v <= 0.5) return (v * v) / 3.0;
    return (exp((v - c) / a) + b) / 12.0;
}

float3 RadioifyHlgEotf(float3 v)
{
    return float3(RadioifyHlgEotf(v.r),
                  RadioifyHlgEotf(v.g),
                  RadioifyHlgEotf(v.b));
}

float RadioifyToneMapFilmic(float x)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) /
            (x * (A * x + B) + D * F)) -
           E / F;
}

float3 RadioifyToneMapFilmic(float3 x)
{
    return float3(RadioifyToneMapFilmic(x.r),
                  RadioifyToneMapFilmic(x.g),
                  RadioifyToneMapFilmic(x.b));
}

float RadioifyLuminance2020(float3 c)
{
    return dot(c, float3(0.2627, 0.6780, 0.0593));
}

#endif
