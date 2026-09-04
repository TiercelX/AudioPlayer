#ifndef DOBYDOWNMIXPROCESSOR_H
#define DOBYDOWNMIXPROCESSOR_H

#include <cstdint>

enum class DolbyDownmixType {
    None,
    LoRo,
    LtRt,
    DplII,
};

struct DolbyDownmixParams {
    DolbyDownmixType type = DolbyDownmixType::None;
    double centerMixLevel = 0.707;
    double centerMixLevelLtRt = 0.707;
    double surroundMixLevel = 0.707;
    double surroundMixLevelLtRt = 0.707;
    double lfeMixLevel = 0.0;
};

class DolbyDownmixProcessor
{
public:
    DolbyDownmixProcessor() = default;

    bool configure(const DolbyDownmixParams &params,
                   int inputChannelCount,
                   int outputChannelCount);

    bool isActive() const;

    int inputChannelCount() const;
    int outputChannelCount() const;

    void processFloat32(const float *inputInterleaved,
                        float *outputInterleaved,
                        int frameCount);

    void processFloat32Planar(const float * const *inputPlanes,
                              float * const *outputPlanes,
                              int frameCount);

private:
    void processFrameFloat32(const float *in, float *out);

    DolbyDownmixParams m_params;
    int m_inputChannelCount = 0;
    int m_outputChannelCount = 0;
    bool m_active = false;

    double m_cmix = 0.707;
    double m_smix = 0.707;
    double m_lfeMix = 0.0;
    bool m_preferLtRt = false;
};

#endif // DOBYDOWNMIXPROCESSOR_H
