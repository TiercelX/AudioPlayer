#ifndef ANDROIDNATIVEAUDIOPLAYER_H
#define ANDROIDNATIVEAUDIOPLAYER_H

#include "nativeaudioplayerstubbase.h"

struct AudioPlayerSourceContext;

class AndroidNativeAudioPlayer : public NativeAudioPlayerStubBase
{
    Q_OBJECT

public:
    explicit AndroidNativeAudioPlayer(QObject *parent = nullptr);

    BackendId backendId() const override;
    QString backendName() const override;

    static bool isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason = nullptr);
};

#endif // ANDROIDNATIVEAUDIOPLAYER_H
