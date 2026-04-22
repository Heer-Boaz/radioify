static const uint RADIOIFY_OUTPUT_COLOR_SDR = 0u;
static const uint RADIOIFY_OUTPUT_COLOR_SCRGB = 1u;
static const uint RADIOIFY_OUTPUT_COLOR_HDR10 = 2u;

static const uint RADIOIFY_TRANSFER_SDR = 0u;
static const uint RADIOIFY_TRANSFER_PQ = 1u;
static const uint RADIOIFY_TRANSFER_HLG = 2u;

static const uint RADIOIFY_MATRIX_BT2020 = 2u;

static const float RADIOIFY_SCRGB_ONE_NITS = 80.0;
static const float RADIOIFY_BT2408_REFERENCE_WHITE_NITS = 203.0;
static const float RADIOIFY_PQ_REFERENCE_MAX_NITS = 10000.0;

float RadioifySafeSdrWhiteNits(float outputSdrWhiteNits)
{
    return max(outputSdrWhiteNits, 1.0);
}

float RadioifyDiffuseWhiteNits(float outputSdrWhiteNits)
{
    return max(RADIOIFY_BT2408_REFERENCE_WHITE_NITS,
               RadioifySafeSdrWhiteNits(outputSdrWhiteNits));
}

float RadioifyGenerateTargetPeakNits(float outputSdrWhiteNits,
                                     float outputPeakNits,
                                     float outputFullFrameNits)
{
    float white = RadioifyDiffuseWhiteNits(outputSdrWhiteNits);
    float policyTarget =
        max(white * 3.5,
            max(outputFullFrameNits, white) * 1.35);
    return max(white + 1.0, min(max(outputPeakNits, white), policyTarget));
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

float3 RadioifyGenerateHdrNits2020(float3 baseNits2020,
                                   float outputSdrWhiteNits,
                                   float outputPeakNits,
                                   float outputFullFrameNits)
{
    const float diffuseWhiteNits =
        RadioifyDiffuseWhiteNits(outputSdrWhiteNits);
    const float targetPeakNits =
        RadioifyGenerateTargetPeakNits(outputSdrWhiteNits, outputPeakNits,
                                       outputFullFrameNits);
    const float midtoneGamma = 1.12;
    const float highlightGain = 2.0;
    const float shoulderStart = diffuseWhiteNits * 0.95;
    const float saturationProtect = 0.22;

    float y = RadioifyLuminance2020(baseNits2020);
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
        (y > 1.0e-6) ? baseNits2020 * (yHdr / y) : baseNits2020;
    float satMix = saturate((yHdr - diffuseWhiteNits) /
                            max(1.0e-4,
                                targetPeakNits - diffuseWhiteNits)) *
                   saturationProtect;
    float gray = RadioifyLuminance2020(outColor);
    return lerp(outColor, gray.xxx, satMix);
}

float3 RadioifySdrVideo709ToBaseNits709(float3 rgb709Code,
                                        float outputSdrWhiteNits)
{
    return RadioifyBt1886ToLinear(rgb709Code) *
           RadioifyDiffuseWhiteNits(outputSdrWhiteNits);
}

float3 RadioifySdrUi709ToBaseNits709(float3 rgb709Code,
                                     float outputSdrWhiteNits)
{
    return RadioifySrgbToLinear(rgb709Code) *
           RadioifyDiffuseWhiteNits(outputSdrWhiteNits);
}

float3 RadioifySdrVideo709ToGeneratedNits2020(float3 rgb709Code,
                                              float outputSdrWhiteNits,
                                              float outputPeakNits,
                                              float outputFullFrameNits)
{
    float3 baseNits2020 =
        RadioifyLinearBt709ToBt2020(
            RadioifySdrVideo709ToBaseNits709(rgb709Code,
                                             outputSdrWhiteNits));
    return RadioifyGenerateHdrNits2020(baseNits2020, outputSdrWhiteNits,
                                       outputPeakNits,
                                       outputFullFrameNits);
}

float3 RadioifySdrUi709ToGeneratedNits2020(float3 rgb709Code,
                                           float outputSdrWhiteNits,
                                           float outputPeakNits,
                                           float outputFullFrameNits)
{
    float3 baseNits2020 =
        RadioifyLinearBt709ToBt2020(
            RadioifySdrUi709ToBaseNits709(rgb709Code,
                                          outputSdrWhiteNits));
    return RadioifyGenerateHdrNits2020(baseNits2020, outputSdrWhiteNits,
                                       outputPeakNits,
                                       outputFullFrameNits);
}

float3 RadioifySdr709ToAsciiNits709(float3 rgb709Code,
                                    float outputSdrWhiteNits,
                                    float asciiGlyphPeakNits,
                                    float emissiveBoost)
{
    float white = RadioifyDiffuseWhiteNits(outputSdrWhiteNits);
    float glyphPeak = max(asciiGlyphPeakNits, white);
    float3 linearRgb = RadioifySrgbToLinear(rgb709Code);
    float3 diffuseNits = linearRgb * white;
    float brightness = max(linearRgb.r, max(linearRgb.g, linearRgb.b));
    float hot = smoothstep(0.35, 1.0, brightness);
    float emissiveAmount = saturate(emissiveBoost - 1.0);
    float peakScale = glyphPeak / white;
    float scale = lerp(1.0, peakScale, hot * emissiveAmount);
    return diffuseNits * scale;
}

float3 RadioifyEncodeNits709ToScRgb(float3 nits709)
{
    return nits709 / RADIOIFY_SCRGB_ONE_NITS;
}

float3 RadioifyEncodeNits709ToSdr(float3 nits709,
                                  float outputSdrWhiteNits)
{
    float white = RadioifySafeSdrWhiteNits(outputSdrWhiteNits);
    float3 linearRgb = max(nits709, float3(0.0, 0.0, 0.0)) / white;
    float mappedWhite = max(RadioifyToneMapFilmic(1.0), 1.0e-4);
    float3 mapped = float3(RadioifyToneMapFilmic(linearRgb.r),
                           RadioifyToneMapFilmic(linearRgb.g),
                           RadioifyToneMapFilmic(linearRgb.b)) /
                    mappedWhite;
    return RadioifyLinearToSrgb(saturate(mapped));
}

float3 RadioifyEncodeNits709ToOutput(float3 nits709, uint outputColorSpace,
                                     float outputSdrWhiteNits)
{
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SCRGB)
        return RadioifyEncodeNits709ToScRgb(nits709);

    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_HDR10)
        return RadioifyPqOetfFromNits(
            RadioifyLinearBt709ToBt2020(
                max(nits709, float3(0.0, 0.0, 0.0))));

    return RadioifyEncodeNits709ToSdr(nits709, outputSdrWhiteNits);
}

float3 RadioifyEncodeNits2020ToOutput(float3 nits2020, uint outputColorSpace,
                                       float outputSdrWhiteNits)
{
    float3 encoded = float3(0.0, 0.0, 0.0);
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_HDR10)
    {
        encoded = RadioifyPqOetfFromNits(
            max(nits2020, float3(0.0, 0.0, 0.0)));
    }
    else
    {
        float3 nits709 = RadioifyLinearBt2020ToBt709(nits2020);
        if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SCRGB)
        {
            encoded = RadioifyEncodeNits709ToScRgb(nits709);
        }
        else
        {
            encoded = RadioifyEncodeNits709ToSdr(nits709,
                                                 outputSdrWhiteNits);
        }
    }
    return encoded;
}

float3 RadioifyHdrTransferToNits2020(float3 rgbCode, uint yuvTransfer,
                                     uint yuvMatrix,
                                     float outputPeakNits)
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
                    max(outputPeakNits, RADIOIFY_BT2408_REFERENCE_WHITE_NITS);
    }

    if (yuvMatrix != RADIOIFY_MATRIX_BT2020)
    {
        linearRgb = RadioifyLinearBt709ToBt2020(linearRgb);
    }
    return max(linearRgb, float3(0.0, 0.0, 0.0));
}

float3 RadioifyVideoToOutput(float3 rgbCode, uint yuvTransfer,
                             uint yuvMatrix, uint outputColorSpace,
                             float outputSdrWhiteNits,
                             float outputPeakNits,
                             float outputFullFrameNits)
{
    float3 outputRgb = float3(0.0, 0.0, 0.0);
    if (outputColorSpace == RADIOIFY_OUTPUT_COLOR_SDR)
    {
        if (yuvTransfer == RADIOIFY_TRANSFER_SDR)
        {
            outputRgb = saturate(rgbCode);
        }
        else
        {
            float3 hdrNits2020 = RadioifyHdrTransferToNits2020(
                saturate(rgbCode), yuvTransfer, yuvMatrix, outputPeakNits);
            outputRgb = RadioifyEncodeNits2020ToOutput(
                hdrNits2020, RADIOIFY_OUTPUT_COLOR_SDR, outputSdrWhiteNits);
        }
    }
    else
    {
        float3 nits2020 = float3(0.0, 0.0, 0.0);
        if (yuvTransfer == RADIOIFY_TRANSFER_SDR)
        {
            nits2020 = RadioifySdrVideo709ToGeneratedNits2020(
                rgbCode, outputSdrWhiteNits, outputPeakNits,
                outputFullFrameNits);
        }
        else
        {
            nits2020 = RadioifyHdrTransferToNits2020(
                saturate(rgbCode), yuvTransfer, yuvMatrix, outputPeakNits);
        }
        outputRgb = RadioifyEncodeNits2020ToOutput(
            nits2020, outputColorSpace, outputSdrWhiteNits);
    }
    return outputRgb;
}
