static const uint RADIOIFY_OUTPUT_COLOR_SDR = 0u;
static const uint RADIOIFY_OUTPUT_COLOR_SCRGB = 1u;
static const uint RADIOIFY_OUTPUT_COLOR_HDR10 = 2u;

static const uint RADIOIFY_TRANSFER_SDR = 0u;
static const uint RADIOIFY_TRANSFER_PQ = 1u;
static const uint RADIOIFY_TRANSFER_HLG = 2u;

static const uint RADIOIFY_MATRIX_BT2020 = 2u;

static const float RADIOIFY_SCRGB_ONE_NITS = 80.0;
static const float RADIOIFY_SDR_REFERENCE_WHITE_NITS = 100.0;
static const float RADIOIFY_BT2408_REFERENCE_WHITE_NITS = 203.0;
static const float RADIOIFY_PQ_REFERENCE_MAX_NITS = 10000.0;

float RadioifySafeOutputMaxNits(float outputMaxNits, float diffuseWhiteNits)
{
    return max(diffuseWhiteNits + 1.0, clamp(outputMaxNits, 400.0, 1200.0));
}

float RadioifySystemSdrWhiteNits(float sdrWhiteScale)
{
    return max(RADIOIFY_SCRGB_ONE_NITS * max(sdrWhiteScale, 0.0),
               RADIOIFY_SCRGB_ONE_NITS);
}

float RadioifyDiffuseWhiteNits(float sdrWhiteScale)
{
    return max(RADIOIFY_BT2408_REFERENCE_WHITE_NITS,
               RadioifySystemSdrWhiteNits(sdrWhiteScale));
}

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
    return (v <= 0.04045) ? (v / 12.92) : pow((v + 0.055) / 1.055, 2.4);
}

float3 RadioifySrgbToLinear(float3 v)
{
    return float3(RadioifySrgbToLinear(v.r),
                  RadioifySrgbToLinear(v.g),
                  RadioifySrgbToLinear(v.b));
}

float RadioifyLinearToSrgb(float v)
{
    v = max(v, 0.0);
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

float3 RadioifyPqOetf(float3 v)
{
    return float3(RadioifyPqOetf(v.r),
                  RadioifyPqOetf(v.g),
                  RadioifyPqOetf(v.b));
}

float RadioifyHlgEotf(float v)
{
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    if (v <= 0.5) return (v * v) / 3.0;
    return (exp((v - c) / a) + b) / 12.0;
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

float RadioifyLuminance2020(float3 c)
{
    return dot(c, float3(0.2627, 0.6780, 0.0593));
}

float3 RadioifyGenerateHdrNits2020(float3 hdrLinear2020Nits,
                                   float sdrWhiteScale,
                                   float outputMaxNits)
{
    const float diffuseWhiteNits = RadioifyDiffuseWhiteNits(sdrWhiteScale);
    const float targetPeakNits =
        RadioifySafeOutputMaxNits(outputMaxNits, diffuseWhiteNits);
    const float midtoneGamma = 1.12;
    const float highlightGain = 2.0;
    const float shoulderStart = diffuseWhiteNits * 0.95;
    const float saturationProtect = 0.22;

    float y = RadioifyLuminance2020(hdrLinear2020Nits);
    float yMid =
        pow(max(y / diffuseWhiteNits, 0.0), 1.0 / midtoneGamma) *
        diffuseWhiteNits;

    float yHdr = yMid;
    if (yMid > shoulderStart)
    {
        float t = max((yMid - shoulderStart) /
                      max(1.0e-4, targetPeakNits - shoulderStart), 0.0);
        float boost = 1.0 + highlightGain * (1.0 - exp(-3.0 * t));
        yHdr = min(targetPeakNits, yMid * boost);
    }

    float3 outColor =
        (y > 1.0e-6) ? hdrLinear2020Nits * (yHdr / y)
                     : hdrLinear2020Nits;

    float satMix = saturate((yHdr - diffuseWhiteNits) /
                            max(1.0e-4,
                                targetPeakNits - diffuseWhiteNits)) *
                   saturationProtect;
    float gray = RadioifyLuminance2020(outColor);
    return lerp(outColor, gray.xxx, satMix);
}

float3 RadioifySdr709ToHdrNits2020(float3 rgb709Code, float sdrWhiteScale,
                                   float outputMaxNits,
                                   float emissiveBoost)
{
    float diffuseWhiteNits = RadioifyDiffuseWhiteNits(sdrWhiteScale);
    float3 linear709 =
        RadioifyBt1886ToLinear(rgb709Code) * max(emissiveBoost, 0.0);
    float3 mapped2020 =
        RadioifyLinearBt709ToBt2020(linear709) * diffuseWhiteNits;
    return RadioifyGenerateHdrNits2020(mapped2020, sdrWhiteScale,
                                       outputMaxNits);
}

float3 RadioifyHdrNits2020ToOutput(float3 hdrNits2020, uint outputColorSpace)
{
    float3 outputRgb = float3(0.0, 0.0, 0.0);
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_HDR10)
    {
        outputRgb =
            RadioifyPqOetf(hdrNits2020 / RADIOIFY_PQ_REFERENCE_MAX_NITS);
    }
    else if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SCRGB)
    {
        outputRgb = max(RadioifyLinearBt2020ToBt709(hdrNits2020) /
                            RADIOIFY_SCRGB_ONE_NITS,
                        float3(0.0, 0.0, 0.0));
    }
    else
    {
        float3 linear709 = max(
            RadioifyLinearBt2020ToBt709(
                hdrNits2020 / RADIOIFY_SDR_REFERENCE_WHITE_NITS),
            float3(0.0, 0.0, 0.0));
        float3 mapped = float3(RadioifyToneMapFilmic(linear709.r),
                               RadioifyToneMapFilmic(linear709.g),
                               RadioifyToneMapFilmic(linear709.b));
        outputRgb = saturate(RadioifyLinearToSrgb(mapped));
    }
    return outputRgb;
}

float3 RadioifySdr709ToOutput(float3 rgb709Code, uint outputColorSpace,
                              float sdrWhiteScale, float outputMaxNits,
                              float emissiveBoost)
{
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SDR)
    {
        return saturate(rgb709Code);
    }
    return RadioifyHdrNits2020ToOutput(
        RadioifySdr709ToHdrNits2020(rgb709Code, sdrWhiteScale,
                                    outputMaxNits, emissiveBoost),
        outputColorSpace);
}

float3 RadioifyHdrTransferToNits2020(float3 rgbCode, uint yuvTransfer,
                                     uint yuvMatrix, float outputMaxNits)
{
    float3 linearRgb = rgbCode;
    if (yuvTransfer == RADIOIFY_TRANSFER_PQ)
    {
        linearRgb = float3(RadioifyPqEotf(rgbCode.r),
                           RadioifyPqEotf(rgbCode.g),
                           RadioifyPqEotf(rgbCode.b)) *
                    RADIOIFY_PQ_REFERENCE_MAX_NITS;
    }
    else if (yuvTransfer == RADIOIFY_TRANSFER_HLG)
    {
        linearRgb = float3(RadioifyHlgEotf(rgbCode.r),
                           RadioifyHlgEotf(rgbCode.g),
                           RadioifyHlgEotf(rgbCode.b)) *
                    max(outputMaxNits, RADIOIFY_BT2408_REFERENCE_WHITE_NITS);
    }

    if (yuvMatrix != RADIOIFY_MATRIX_BT2020)
    {
        linearRgb = RadioifyLinearBt709ToBt2020(linearRgb);
    }
    return max(linearRgb, 0.0);
}

float3 RadioifyVideoToOutput(float3 rgbCode, uint yuvTransfer,
                             uint yuvMatrix, uint outputColorSpace,
                             float sdrWhiteScale, float outputMaxNits)
{
    float3 outputRgb = float3(0.0, 0.0, 0.0);
    if (yuvTransfer == RADIOIFY_TRANSFER_SDR)
    {
        outputRgb = RadioifySdr709ToOutput(rgbCode, outputColorSpace,
                                           sdrWhiteScale, outputMaxNits, 1.0);
    }
    else
    {
        float3 hdrNits2020 =
            RadioifyHdrTransferToNits2020(saturate(rgbCode), yuvTransfer,
                                          yuvMatrix, outputMaxNits);
        outputRgb = RadioifyHdrNits2020ToOutput(hdrNits2020,
                                                outputColorSpace);
    }
    return outputRgb;
}
