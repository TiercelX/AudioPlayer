#include "dolbydownmixprocessor.h"

#include <algorithm>
#include <cstring>

bool DolbyDownmixProcessor::configure(const DolbyDownmixParams &params,
                                      int inputChannelCount,
                                      int outputChannelCount)
{
    m_params = params;
    m_inputChannelCount = inputChannelCount;
    m_outputChannelCount = outputChannelCount;
    m_active = false;

    if (m_params.type == DolbyDownmixType::None) {
        return false;
    }

    if (outputChannelCount != 2) {
        return false;
    }

    if (inputChannelCount < 3) {
        return false;
    }

    m_cmix = (m_params.type == DolbyDownmixType::LtRt || m_params.type == DolbyDownmixType::DplII)
        ? m_params.centerMixLevelLtRt
        : m_params.centerMixLevel;
    m_smix = (m_params.type == DolbyDownmixType::LtRt || m_params.type == DolbyDownmixType::DplII)
        ? m_params.surroundMixLevelLtRt
        : m_params.surroundMixLevel;
    m_lfeMix = m_params.lfeMixLevel;
    m_preferLtRt = (m_params.type == DolbyDownmixType::LtRt
                    || m_params.type == DolbyDownmixType::DplII);

    m_active = true;
    return true;
}

bool DolbyDownmixProcessor::isActive() const
{
    return m_active;
}

int DolbyDownmixProcessor::inputChannelCount() const
{
    return m_inputChannelCount;
}

int DolbyDownmixProcessor::outputChannelCount() const
{
    return m_outputChannelCount;
}

void DolbyDownmixProcessor::processFrameFloat32(const float *in, float *out)
{
    const int ch = m_inputChannelCount;

    float L = in[0];
    float R = in[1];
    float C = (ch > 2) ? in[2] : 0.0f;
    float LFE = (ch > 3) ? in[3] : 0.0f;
    float Ls = (ch > 4) ? in[4] : 0.0f;
    float Rs = (ch > 5) ? in[5] : 0.0f;
    float Lb = (ch > 6) ? in[6] : 0.0f;
    float Rb = (ch > 7) ? in[7] : 0.0f;

    const float cmix = static_cast<float>(m_cmix);
    const float smix = static_cast<float>(m_smix);
    const float lfeMix = static_cast<float>(m_lfeMix);

    float outL = L + cmix * C + lfeMix * LFE;
    float outR = R + cmix * C + lfeMix * LFE;

    if (m_preferLtRt) {
        outL += smix * Ls - smix * Rs + smix * Lb - smix * Rb;
        outR += -smix * Ls + smix * Rs - smix * Lb + smix * Rb;
    } else {
        outL += smix * Ls + smix * Lb;
        outR += smix * Rs + smix * Rb;
    }

    out[0] = outL;
    out[1] = outR;
}

void DolbyDownmixProcessor::processFloat32(const float *inputInterleaved,
                                           float *outputInterleaved,
                                           int frameCount)
{
    if (!m_active) {
        return;
    }

    const int inCh = m_inputChannelCount;
    const int outCh = m_outputChannelCount;

    for (int i = 0; i < frameCount; ++i) {
        processFrameFloat32(inputInterleaved + i * inCh,
                            outputInterleaved + i * outCh);
    }
}

void DolbyDownmixProcessor::processFloat32Planar(const float * const *inputPlanes,
                                                  float * const *outputPlanes,
                                                  int frameCount)
{
    if (!m_active) {
        return;
    }

    const int ch = m_inputChannelCount;

    for (int i = 0; i < frameCount; ++i) {
        float L = inputPlanes[0][i];
        float R = inputPlanes[1][i];
        float C = (ch > 2) ? inputPlanes[2][i] : 0.0f;
        float LFE = (ch > 3) ? inputPlanes[3][i] : 0.0f;
        float Ls = (ch > 4) ? inputPlanes[4][i] : 0.0f;
        float Rs = (ch > 5) ? inputPlanes[5][i] : 0.0f;
        float Lb = (ch > 6) ? inputPlanes[6][i] : 0.0f;
        float Rb = (ch > 7) ? inputPlanes[7][i] : 0.0f;

        const float cmix = static_cast<float>(m_cmix);
        const float smix = static_cast<float>(m_smix);
        const float lfeMix = static_cast<float>(m_lfeMix);

        float outL = L + cmix * C + lfeMix * LFE;
        float outR = R + cmix * C + lfeMix * LFE;

        if (m_preferLtRt) {
            outL += smix * Ls - smix * Rs + smix * Lb - smix * Rb;
            outR += -smix * Ls + smix * Rs - smix * Lb + smix * Rb;
        } else {
            outL += smix * Ls + smix * Lb;
            outR += smix * Rs + smix * Rb;
        }

        outputPlanes[0][i] = outL;
        outputPlanes[1][i] = outR;
    }
}
