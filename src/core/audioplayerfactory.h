#ifndef AUDIOPLAYERFACTORY_H
#define AUDIOPLAYERFACTORY_H

#include "audioplayerbackend.h"

#include <QString>

class QObject;

struct AudioPlayerSourceContext
{
    QString filePath;
    QString codecName;
    int sourceChannelCount = 0;
};

struct AudioPlaybackPlan
{
    enum class SourceMode {
        OriginalFile,
        RemuxRawDolbySidecar,
    };

    AudioPlayerBackend::BackendId backendId = AudioPlayerBackend::BackendId::Ffmpeg;
    SourceMode sourceMode = SourceMode::OriginalFile;
};

class AudioPlayerFactory
{
public:
    static AudioPlaybackPlan buildPlaybackPlan(const AudioPlayerSourceContext &context);
    static AudioPlayerBackend::BackendId selectBackend(const AudioPlayerSourceContext &context);
    static AudioPlayerBackend *create(AudioPlayerBackend::BackendId backendId, QObject *parent = nullptr);
};

#endif // AUDIOPLAYERFACTORY_H
