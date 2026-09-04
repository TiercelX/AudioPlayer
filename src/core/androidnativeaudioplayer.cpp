#include "androidnativeaudioplayer.h"

#include "audioplayerfactory.h"

#include <QObject>

AndroidNativeAudioPlayer::AndroidNativeAudioPlayer(QObject *parent)
    : NativeAudioPlayerStubBase(parent)
{
}

AudioPlayerBackend::BackendId AndroidNativeAudioPlayer::backendId() const
{
    return BackendId::AndroidNative;
}

QString AndroidNativeAudioPlayer::backendName() const
{
    return tr("Android Native (skeleton)");
}

bool AndroidNativeAudioPlayer::isSupportedForContext(const AudioPlayerSourceContext &context, QString *reason)
{
    Q_UNUSED(context);

#if defined(Q_OS_ANDROID)
    if (reason) {
        *reason = QObject::tr("Android native backend skeleton is wired, but MediaCodec playback is not implemented yet.");
    }
#else
    if (reason) {
        *reason = QObject::tr("Current build is not targeting Android.");
    }
#endif

    return false;
}
