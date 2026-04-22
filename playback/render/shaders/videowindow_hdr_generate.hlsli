static const uint RADIOIFY_OUTPUT_COLOR_SDR = 0u;
static const uint RADIOIFY_OUTPUT_COLOR_SCRGB = 1u;
static const uint RADIOIFY_OUTPUT_COLOR_HDR10 = 2u;

static const uint RADIOIFY_TRANSFER_SDR = 0u;
static const uint RADIOIFY_TRANSFER_PQ = 1u;
static const uint RADIOIFY_TRANSFER_HLG = 2u;

static const uint RADIOIFY_MATRIX_BT2020 = 2u;

static const float RADIOIFY_SCRGB_ONE_NITS = 80.0;
static const float RADIOIFY_BT2408_REFERENCE_WHITE_NITS = 203.0;

#include "video_color_math.hlsli"

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
