#include "audioplayerfactory.h"

#include "androidnativeaudioplayer.h"
#include "applenativeaudioplayer.h"
#include "ffmpegaudioplayer.h"
#include "playerlogger.h"
#if defined(Q_OS_WINDOWS)
#include "windowsasioaudioplayer.h"
#include "windowswasapiaudioplayer.h"
#endif
#if defined(Q_OS_LINUX)
#include "linuxalsaaudioplayer.h"
#endif

namespace {

bool hasRawDolbyExtension(const QString &filePath)
{
    const QString lowerPath = filePath.toLower();
    return lowerPath.endsWith(QStringLiteral(".eb3"))
        || lowerPath.endsWith(QStringLiteral(".ec3"))
        || lowerPath.endsWith(QStringLiteral(".mlp"));
}

bool isDolbyCodec(const QString &codecName)
{
    const QString lowerCodecName = codecName.toLower();
    return lowerCodecName == QStringLiteral("eac3")
        || lowerCodecName == QStringLiteral("ac3")
        || lowerCodecName == QStringLiteral("truehd");
}

QString backendIdName(AudioPlayerBackend::BackendId backendId)
{
    switch (backendId) {
    case AudioPlayerBackend::BackendId::Ffmpeg:
        return QStringLiteral("ffmpeg");
    case AudioPlayerBackend::BackendId::WindowsWasapi:
        return QStringLiteral("windows-wasapi");
    case AudioPlayerBackend::BackendId::WindowsAsio:
        return QStringLiteral("windows-asio");
    case AudioPlayerBackend::BackendId::AppleNative:
        return QStringLiteral("apple-native");
    case AudioPlayerBackend::BackendId::AndroidNative:
        return QStringLiteral("android-native");
    case AudioPlayerBackend::BackendId::LinuxAlsa:
        return QStringLiteral("linux-alsa");
    }

    return QStringLiteral("unknown");
}

} // namespace

AudioPlayerBackend::BackendId AudioPlayerFactory::selectBackend(const AudioPlayerSourceContext &context)
{
    return buildPlaybackPlan(context).backendId;
}

AudioPlaybackPlan AudioPlayerFactory::buildPlaybackPlan(const AudioPlayerSourceContext &context)
{
    Q_UNUSED(context.sourceChannelCount);

    AudioPlaybackPlan plan;

#if defined(Q_OS_WINDOWS)
    const auto defaultBackendId = AudioPlayerBackend::BackendId::WindowsWasapi;
#else
    const auto defaultBackendId = AudioPlayerBackend::BackendId::Ffmpeg;
#endif

    // Keep source-aware playback planning localized here so platform-specific
    // routing and source preparation rules can evolve without spreading into
    // the UI layer.
    if (hasRawDolbyExtension(context.filePath)) {
        plan.backendId = defaultBackendId;
        plan.sourceMode = AudioPlaybackPlan::SourceMode::RemuxRawDolbySidecar;
        return plan;
    }

#if defined(Q_OS_ANDROID)
    QString reason;
    if (AndroidNativeAudioPlayer::isSupportedForContext(context, &reason)) {
        plan.backendId = AudioPlayerBackend::BackendId::AndroidNative;
        PlayerLogger::log(QStringLiteral("factory"),
                          QStringLiteral("buildPlaybackPlan selected=%1 codec=%2 path=%3")
                              .arg(backendIdName(plan.backendId))
                              .arg(context.codecName)
                              .arg(context.filePath));
        return plan;
    }

    PlayerLogger::log(QStringLiteral("factory"),
                      QStringLiteral("buildPlaybackPlan fallback=%1 requested=%2 codec=%3 path=%4 reason=%5")
                          .arg(backendIdName(AudioPlayerBackend::BackendId::Ffmpeg))
                          .arg(backendIdName(AudioPlayerBackend::BackendId::AndroidNative))
                          .arg(context.codecName)
                          .arg(context.filePath)
                          .arg(reason));
#elif defined(Q_OS_IOS) || defined(Q_OS_MACOS)
    QString reason;
    if (AppleNativeAudioPlayer::isSupportedForContext(context, &reason)) {
        plan.backendId = AudioPlayerBackend::BackendId::AppleNative;
        PlayerLogger::log(QStringLiteral("factory"),
                          QStringLiteral("buildPlaybackPlan selected=%1 codec=%2 path=%3")
                              .arg(backendIdName(plan.backendId))
                              .arg(context.codecName)
                              .arg(context.filePath));
        return plan;
    }

    PlayerLogger::log(QStringLiteral("factory"),
                      QStringLiteral("buildPlaybackPlan fallback=%1 requested=%2 codec=%3 path=%4 reason=%5")
                          .arg(backendIdName(AudioPlayerBackend::BackendId::Ffmpeg))
                          .arg(backendIdName(AudioPlayerBackend::BackendId::AppleNative))
                          .arg(context.codecName)
                          .arg(context.filePath)
                          .arg(reason));
#elif defined(Q_OS_LINUX)
    QString reason;
    if (LinuxAlsaAudioPlayer::isSupportedForContext(context, &reason)) {
        plan.backendId = AudioPlayerBackend::BackendId::LinuxAlsa;
        PlayerLogger::log(QStringLiteral("factory"),
                          QStringLiteral("buildPlaybackPlan selected=%1 codec=%2 path=%3")
                              .arg(backendIdName(plan.backendId))
                              .arg(context.codecName)
                              .arg(context.filePath));
        return plan;
    }

    PlayerLogger::log(QStringLiteral("factory"),
                      QStringLiteral("buildPlaybackPlan fallback=%1 requested=%2 codec=%3 path=%4 reason=%5")
                          .arg(backendIdName(AudioPlayerBackend::BackendId::Ffmpeg))
                          .arg(backendIdName(AudioPlayerBackend::BackendId::LinuxAlsa))
                          .arg(context.codecName)
                          .arg(context.filePath)
                          .arg(reason));
#elif defined(Q_OS_WINDOWS)
    QString reason;
    if (WindowsWasapiAudioPlayer::isSupportedForContext(context, &reason)) {
        plan.backendId = AudioPlayerBackend::BackendId::WindowsWasapi;
        PlayerLogger::log(QStringLiteral("factory"),
                          QStringLiteral("buildPlaybackPlan selected=%1 codec=%2 path=%3")
                              .arg(backendIdName(plan.backendId))
                              .arg(context.codecName)
                              .arg(context.filePath));
        return plan;
    }

    PlayerLogger::log(QStringLiteral("factory"),
                      QStringLiteral("buildPlaybackPlan fallback=%1 requested=%2 codec=%3 path=%4 reason=%5")
                          .arg(backendIdName(AudioPlayerBackend::BackendId::Ffmpeg))
                          .arg(backendIdName(AudioPlayerBackend::BackendId::WindowsWasapi))
                          .arg(context.codecName)
                          .arg(context.filePath)
                          .arg(reason));
#else
    if (isDolbyCodec(context.codecName)) {
        PlayerLogger::log(QStringLiteral("factory"),
                          QStringLiteral("buildPlaybackPlan keep=%1 codec=%2 path=%3")
                              .arg(backendIdName(defaultBackendId))
                              .arg(context.codecName)
                              .arg(context.filePath));
    }
#endif

    plan.backendId = defaultBackendId;
    return plan;
}

AudioPlayerBackend *AudioPlayerFactory::create(AudioPlayerBackend::BackendId backendId, QObject *parent)
{
    switch (backendId) {
    case AudioPlayerBackend::BackendId::Ffmpeg:
        return new FfmpegAudioPlayer(parent);
    case AudioPlayerBackend::BackendId::WindowsWasapi:
#if defined(Q_OS_WINDOWS)
        return new WindowsWasapiAudioPlayer(parent);
#else
        return new FfmpegAudioPlayer(parent);
#endif
    case AudioPlayerBackend::BackendId::WindowsAsio:
#if defined(Q_OS_WINDOWS)
        return new WindowsAsioAudioPlayer(parent);
#else
        return new FfmpegAudioPlayer(parent);
#endif
    case AudioPlayerBackend::BackendId::AppleNative:
        return new AppleNativeAudioPlayer(parent);
    case AudioPlayerBackend::BackendId::AndroidNative:
        return new AndroidNativeAudioPlayer(parent);
    case AudioPlayerBackend::BackendId::LinuxAlsa:
#if defined(Q_OS_LINUX)
        return new LinuxAlsaAudioPlayer(parent);
#else
        return new FfmpegAudioPlayer(parent);
#endif
    }

    return new FfmpegAudioPlayer(parent);
}
