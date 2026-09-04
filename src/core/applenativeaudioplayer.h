#ifndef APPLENATIVEAUDIOPLAYER_H
#define APPLENATIVEAUDIOPLAYER_H

#include "nativeaudioplayerstubbase.h"

struct AudioPlayerSourceContext;

class AppleNativeAudioPlayer : public NativeAudioPlayerStubBase
{
    Q_OBJECT

public:
    explicit AppleNativeAudioPlayer(QObject *parent = nullptr);

    BackendId backendId() const override;
    QString backendName() const override;

    static bool isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason = nullptr);
};

#endif // APPLENATIVEAUDIOPLAYER_H
