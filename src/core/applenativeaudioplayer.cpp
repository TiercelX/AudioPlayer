#include "applenativeaudioplayer.h"

#include "audioplayerfactory.h"

#include <QObject>

AppleNativeAudioPlayer::AppleNativeAudioPlayer(QObject *parent)
    : NativeAudioPlayerStubBase(parent)
{
}

AudioPlayerBackend::BackendId AppleNativeAudioPlayer::backendId() const
{
    return BackendId::AppleNative;
}

QString AppleNativeAudioPlayer::backendName() const
{
    return tr("Apple Native (skeleton)");
}

bool AppleNativeAudioPlayer::isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason)
{
    Q_UNUSED(context);

#if defined(Q_OS_IOS) || defined(Q_OS_MACOS)
    if (reason) {
        *reason = QObject::tr("Apple native backend skeleton is wired, but AVFoundation playback is not implemented yet.");
    }
#else
    if (reason) {
        *reason = QObject::tr("Current build is not targeting an Apple platform.");
    }
#endif

    return false;
}
