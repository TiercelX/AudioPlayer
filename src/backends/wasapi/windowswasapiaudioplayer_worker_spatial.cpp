#include "windowswasapiaudioplayer_worker.h"

namespace {

QString spatialObjectTypeName(AudioObjectType type)
{
    switch (type) {
    case AudioObjectType_FrontLeft:
        return QStringLiteral("FL");
    case AudioObjectType_FrontRight:
        return QStringLiteral("FR");
    case AudioObjectType_FrontCenter:
        return QStringLiteral("FC");
    case AudioObjectType_LowFrequency:
        return QStringLiteral("LFE");
    case AudioObjectType_SideLeft:
        return QStringLiteral("SL");
    case AudioObjectType_SideRight:
        return QStringLiteral("SR");
    case AudioObjectType_TopFrontLeft:
        return QStringLiteral("TFL");
    case AudioObjectType_TopFrontRight:
        return QStringLiteral("TFR");
    default:
        break;
    }
    return QStringLiteral("unknown");
}

QString spatialMaskNames(AudioObjectType mask)
{
    const AudioObjectType types[] = {
        AudioObjectType_FrontLeft,
        AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter,
        AudioObjectType_LowFrequency,
        AudioObjectType_SideLeft,
        AudioObjectType_SideRight,
        AudioObjectType_TopFrontLeft,
        AudioObjectType_TopFrontRight,
    };
    QStringList names;
    for (const AudioObjectType type : types) {
        if ((mask & type) == type) {
            names << spatialObjectTypeName(type);
        }
    }
    return names.join(QLatin1Char(','));
}

bool isMonoFloatFormat(const WAVEFORMATEX *format)
{
    if (!format || format->nChannels != 1 || format->nSamplesPerSec <= 0
        || format->wBitsPerSample != 32) {
        return false;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        return extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    return false;
}

} // namespace

bool WasapiOutputWorker::configureSpatialStaticBed(const QAudioDevice &device)
{
    if (!m_device || !m_bufferFormat.isValid() || m_bufferFormat.channelCount != 8
        || m_bufferFormat.channelLayout != QStringLiteral("5.1.2")) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=unsupported-buffer-format channels=%2 layout=%3")
                              .arg(m_sessionId)
                              .arg(m_bufferFormat.channelCount)
                              .arg(m_bufferFormat.channelLayout));
        return false;
    }

    HRESULT hr = m_device->Activate(__uuidof(ISpatialAudioClient),
                                    CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void **>(&m_spatialClient));
    if (FAILED(hr) || !m_spatialClient) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=activate-client hr=0x%2")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        releaseSpatialStaticBed();
        return false;
    }

    hr = m_spatialClient->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr);
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=stream-unavailable hr=0x%2")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        releaseSpatialStaticBed();
        return false;
    }

    const AudioObjectType staticMask =
        static_cast<AudioObjectType>(AudioObjectType_FrontLeft
                                     | AudioObjectType_FrontRight
                                     | AudioObjectType_FrontCenter
                                     | AudioObjectType_LowFrequency
                                     | AudioObjectType_SideLeft
                                     | AudioObjectType_SideRight
                                     | AudioObjectType_TopFrontLeft
                                     | AudioObjectType_TopFrontRight);
    AudioObjectType nativeMask = AudioObjectType_None;
    hr = m_spatialClient->GetNativeStaticObjectTypeMask(&nativeMask);
    if (FAILED(hr) || (nativeMask & staticMask) != staticMask) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=unsupported-static-mask hr=0x%2 native=%3 required=%4")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(spatialMaskNames(nativeMask))
                              .arg(spatialMaskNames(staticMask)));
        releaseSpatialStaticBed();
        return false;
    }

    IAudioFormatEnumerator *formatEnumerator = nullptr;
    hr = m_spatialClient->GetSupportedAudioObjectFormatEnumerator(&formatEnumerator);
    if (FAILED(hr) || !formatEnumerator) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=format-enumerator hr=0x%2")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        safeRelease(formatEnumerator);
        releaseSpatialStaticBed();
        return false;
    }

    WAVEFORMATEX *selectedFormat = nullptr;
    UINT32 formatCount = 0;
    formatEnumerator->GetCount(&formatCount);
    for (UINT32 index = 0; index < formatCount; ++index) {
        WAVEFORMATEX *candidate = nullptr;
        if (SUCCEEDED(formatEnumerator->GetFormat(index, &candidate)) && isMonoFloatFormat(candidate)) {
            selectedFormat = candidate;
            break;
        }
        if (candidate) {
            CoTaskMemFree(candidate);
        }
    }
    safeRelease(formatEnumerator);
    if (!selectedFormat) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=no-mono-float-format formatCount=%2")
                              .arg(m_sessionId)
                              .arg(formatCount));
        releaseSpatialStaticBed();
        return false;
    }

    const int spatialFormatBytes = sizeof(WAVEFORMATEX) + selectedFormat->cbSize;
    m_spatialObjectFormatData =
        QByteArray(reinterpret_cast<const char *>(selectedFormat), spatialFormatBytes);
    auto *objectFormat = reinterpret_cast<const WAVEFORMATEX *>(m_spatialObjectFormatData.constData());
    hr = m_spatialClient->IsAudioObjectFormatSupported(objectFormat);
    if (FAILED(hr)) {
        CoTaskMemFree(selectedFormat);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=object-format-unsupported hr=0x%2 rate=%3 channels=%4 bits=%5")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(objectFormat->nSamplesPerSec)
                              .arg(objectFormat->nChannels)
                              .arg(objectFormat->wBitsPerSample));
        releaseSpatialStaticBed();
        return false;
    }

    m_spatialEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_spatialEvent) {
        CoTaskMemFree(selectedFormat);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=create-event error=%2")
                              .arg(m_sessionId)
                              .arg(GetLastError()));
        releaseSpatialStaticBed();
        return false;
    }

    SpatialAudioObjectRenderStreamActivationParams params = {};
    params.ObjectFormat = objectFormat;
    params.StaticObjectTypeMask = staticMask;
    params.MinDynamicObjectCount = 0;
    params.MaxDynamicObjectCount = 0;
    params.Category = AudioCategory_Media;
    params.EventHandle = m_spatialEvent;
    params.NotifyObject = nullptr;

    PROPVARIANT activationParams;
    PropVariantInit(&activationParams);
    activationParams.vt = VT_BLOB;
    activationParams.blob.cbSize = sizeof(params);
    activationParams.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
    hr = m_spatialClient->ActivateSpatialAudioStream(&activationParams,
                                                     __uuidof(ISpatialAudioObjectRenderStream),
                                                     reinterpret_cast<void **>(&m_spatialStream));
    CoTaskMemFree(selectedFormat);
    if (FAILED(hr) || !m_spatialStream) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed unavailable session=%1 reason=activate-stream hr=0x%2")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        releaseSpatialStaticBed();
        return false;
    }

    const AudioObjectType objectTypes[] = {
        AudioObjectType_FrontLeft,
        AudioObjectType_FrontRight,
        AudioObjectType_FrontCenter,
        AudioObjectType_LowFrequency,
        AudioObjectType_SideLeft,
        AudioObjectType_SideRight,
        AudioObjectType_TopFrontLeft,
        AudioObjectType_TopFrontRight,
    };
    m_spatialObjects.clear();
    m_spatialObjectTypes.clear();
    for (const AudioObjectType type : objectTypes) {
        m_spatialObjects.append(nullptr);
        m_spatialObjectTypes.append(type);
    }

    m_bufferFrameCount = 0;
    UINT32 maxFrameCount = 0;
    if (SUCCEEDED(m_spatialClient->GetMaxFrameCount(objectFormat, &maxFrameCount))) {
        m_bufferFrameCount = maxFrameCount;
    }
    if (m_bufferFrameCount == 0) {
        m_bufferFrameCount = 1024;
    }
    m_deviceFormat = m_bufferFormat;
    m_deviceFormat.sampleRate = static_cast<int>(objectFormat->nSamplesPerSec);
    m_deviceFormat.sampleEncoding = PcmSampleEncoding::Float32;
    m_deviceFormat.validBitsPerSample = 32;

    m_spatialEventNotifier = new QWinEventNotifier(m_spatialEvent, this);
    m_spatialEventNotifier->setEnabled(false);
    connect(m_spatialEventNotifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
        renderAvailableFrames();
    });
    m_spatialStaticBedActive = true;
    m_exclusiveModeActive = false;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialStaticBed configured session=%1 device=%2 layout=%3 objectRate=%4 objectChannels=%5 objectBits=%6 staticMask=%7 maxFrameCount=%8")
                          .arg(m_sessionId)
                          .arg(device.description())
                          .arg(m_bufferFormat.channelLayout)
                          .arg(objectFormat->nSamplesPerSec)
                          .arg(objectFormat->nChannels)
                          .arg(objectFormat->wBitsPerSample)
                          .arg(spatialMaskNames(staticMask))
                          .arg(m_bufferFrameCount));
    return true;
}

bool WasapiOutputWorker::ensureSpatialStaticObjectsActivated()
{
    if (!m_spatialStream || m_spatialObjectTypes.isEmpty()) {
        return false;
    }

    bool allActivated = true;
    for (ISpatialAudioObject *object : m_spatialObjects) {
        allActivated = allActivated && object;
    }
    if (allActivated) {
        return true;
    }

    for (int index = 0; index < m_spatialObjectTypes.size(); ++index) {
        if (m_spatialObjects[index]) {
            continue;
        }

        ISpatialAudioObject *object = nullptr;
        const AudioObjectType type = m_spatialObjectTypes[index];
        const HRESULT hr = m_spatialStream->ActivateSpatialAudioObject(type, &object);
        if (FAILED(hr) || !object) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("spatialStaticBed activateObjectFailed session=%1 objectIndex=%2 objectType=%3 hr=0x%4")
                                  .arg(m_sessionId)
                                  .arg(index)
                                  .arg(spatialObjectTypeName(type))
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16)));
            safeRelease(object);
            return false;
        }
        m_spatialObjects[index] = object;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialStaticBed objectsActivated session=%1 count=%2")
                          .arg(m_sessionId)
                          .arg(m_spatialObjects.size()));
    return true;
}

void WasapiOutputWorker::releaseSpatialStaticBed()
{
    if (m_spatialEventNotifier) {
        m_spatialEventNotifier->setEnabled(false);
        delete m_spatialEventNotifier;
        m_spatialEventNotifier = nullptr;
    }
    if (m_spatialStream && m_started) {
        m_spatialStream->Stop();
    }
    for (ISpatialAudioObject *&object : m_spatialObjects) {
        safeRelease(object);
    }
    m_spatialObjects.clear();
    m_spatialObjectTypes.clear();
    if (m_spatialStream) {
        m_spatialStream->Reset();
    }
    safeRelease(m_spatialStream);
    safeRelease(m_spatialClient);
    if (m_spatialEvent) {
        CloseHandle(m_spatialEvent);
        m_spatialEvent = nullptr;
    }
    m_spatialObjectFormatData.clear();
    m_spatialStaticBedActive = false;
}

bool WasapiOutputWorker::writeSpatialSilence(UINT32 frameCount, const QString &reason)
{
    if (!m_spatialStream || frameCount == 0) {
        return false;
    }

    UINT32 availableDynamicObjects = 0;
    UINT32 frameCountPerBuffer = 0;
    HRESULT hr = m_spatialStream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCountPerBuffer);
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed beginUpdateFailed session=%1 reason=%2 hr=0x%3")
                              .arg(m_sessionId)
                              .arg(reason)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        handleFatalError(hr, false);
        return false;
    }
    const UINT32 frames = qMin(frameCount, frameCountPerBuffer);
    if (!ensureSpatialStaticObjectsActivated()) {
        m_spatialStream->EndUpdatingAudioObjects();
        handleFatalError(E_FAIL, false);
        return false;
    }
    for (int index = 0; index < m_spatialObjects.size(); ++index) {
        ISpatialAudioObject *object = m_spatialObjects[index];
        BYTE *buffer = nullptr;
        UINT32 bufferLength = 0;
        hr = object->GetBuffer(&buffer, &bufferLength);
        if (FAILED(hr)) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("spatialStaticBed objectBufferFailed session=%1 reason=%2 objectIndex=%3 objectType=%4 hr=0x%5")
                                  .arg(m_sessionId)
                                  .arg(reason)
                                  .arg(index)
                                  .arg(spatialObjectTypeName(m_spatialObjectTypes[index]))
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16)));
            m_spatialStream->EndUpdatingAudioObjects();
            handleFatalError(hr, false);
            return false;
        }
        std::memset(buffer, 0, static_cast<size_t>(bufferLength));
    }
    hr = m_spatialStream->EndUpdatingAudioObjects();
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed endUpdateFailed session=%1 reason=%2 hr=0x%3")
                              .arg(m_sessionId)
                              .arg(reason)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        handleFatalError(hr, false);
        return false;
    }
    m_submittedFrames += frames;
    m_waitForDataStreak = 0;
    observeArtifactSilence(frames, 0, frameCountPerBuffer, false, reason);
    return true;
}

bool WasapiOutputWorker::writeSpatialPcm(const QByteArray &chunk, UINT32 frameCount)
{
    if (!m_spatialStream || m_spatialObjects.size() != 8 || frameCount == 0
        || !m_bufferFormat.isValid() || m_bufferFormat.channelCount != 8) {
        return false;
    }

    UINT32 availableDynamicObjects = 0;
    UINT32 frameCountPerBuffer = 0;
    HRESULT hr = m_spatialStream->BeginUpdatingAudioObjects(&availableDynamicObjects, &frameCountPerBuffer);
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed beginUpdateFailed session=%1 reason=pcm hr=0x%2")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16)));
        handleFatalError(hr, false);
        return false;
    }
    if (!ensureSpatialStaticObjectsActivated()) {
        m_spatialStream->EndUpdatingAudioObjects();
        handleFatalError(E_FAIL, false);
        return false;
    }

    const UINT32 frames = qMin(frameCount, frameCountPerBuffer);
    const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
    const int bytesPerSample = m_bufferFormat.bytesPerSample();
    const char *source = chunk.constData();
    for (int channel = 0; channel < m_spatialObjects.size(); ++channel) {
        BYTE *objectBuffer = nullptr;
        UINT32 objectBufferLength = 0;
        hr = m_spatialObjects[channel]->GetBuffer(&objectBuffer, &objectBufferLength);
        if (FAILED(hr)) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("spatialStaticBed objectBufferFailed session=%1 reason=pcm objectIndex=%2 objectType=%3 hr=0x%4")
                                  .arg(m_sessionId)
                                  .arg(channel)
                                  .arg(spatialObjectTypeName(m_spatialObjectTypes[channel]))
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16)));
            m_spatialStream->EndUpdatingAudioObjects();
            handleFatalError(hr, false);
            return false;
        }
        auto *target = reinterpret_cast<float *>(objectBuffer);
        const UINT32 objectFrames = qMin<UINT32>(frames, objectBufferLength / sizeof(float));
        for (UINT32 frame = 0; frame < objectFrames; ++frame) {
            const char *sampleData = source
                + static_cast<qsizetype>(frame) * bytesPerFrame
                + channel * bytesPerSample;
            target[frame] = static_cast<float>(readNormalizedSample(sampleData, m_bufferFormat));
        }
        if (objectFrames * sizeof(float) < objectBufferLength) {
            std::memset(reinterpret_cast<char *>(objectBuffer) + objectFrames * sizeof(float),
                        0,
                        objectBufferLength - objectFrames * sizeof(float));
        }
    }

    hr = m_spatialStream->EndUpdatingAudioObjects();
    if (FAILED(hr)) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialStaticBed endUpdateFailed session=%1 reason=pcm hr=0x%2 frames=%3 frameCountPerBuffer=%4")
                              .arg(m_sessionId)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(frames)
                              .arg(frameCountPerBuffer));
        handleFatalError(hr, false);
        return false;
    }
    return true;
}

void WasapiOutputWorker::renderSpatialFrames()
{
    if (m_terminalErrorLatched || !m_started || !m_spatialStream || !m_buffer
        || !m_bufferFormat.isValid()) {
        return;
    }
    if (!m_buffer->matchesOwner(m_sessionId, m_outputBufferGeneration)) {
        bool staleRead = false;
        m_buffer->readForOwner(0, m_sessionId, m_outputBufferGeneration, &staleRead);
        return;
    }

    const UINT32 frameCountPerBuffer = m_bufferFrameCount;
    if (frameCountPerBuffer == 0) {
        return;
    }

    const int bytesPerFrame = m_bufferFormat.bytesPerFrame();
    const UINT32 bufferedFrames = static_cast<UINT32>(m_buffer->bufferedBytes() / bytesPerFrame);
    const bool endOfStream = m_buffer->endOfStream();
    if (bufferedFrames == 0 && endOfStream) {
        m_idleSignaled = true;
        emit stateChanged(m_sessionId,
                          static_cast<int>(QAudio::IdleState),
                          static_cast<int>(QtAudio::NoError));
        return;
    }

    if (m_pendingStartupSilenceFrames > 0) {
        const UINT32 silentFrames = qMin(frameCountPerBuffer, m_pendingStartupSilenceFrames);
        if (writeSpatialSilence(silentFrames, QStringLiteral("spatial-static-bed-startup-silence"))) {
            m_pendingStartupSilenceFrames = silentFrames >= m_pendingStartupSilenceFrames
                ? 0
                : m_pendingStartupSilenceFrames - silentFrames;
        }
        return;
    }

    if (m_pendingRecoveryWarmupFrames > 0) {
        const UINT32 discardFrames = qMin(qMin(frameCountPerBuffer, bufferedFrames), m_pendingRecoveryWarmupFrames);
        if (discardFrames == 0) {
            return;
        }
        const QByteArray discarded = m_buffer->readForOwner(static_cast<qint64>(discardFrames) * bytesPerFrame,
                                                           m_sessionId,
                                                           m_outputBufferGeneration);
        const UINT32 discardedFrames = static_cast<UINT32>(discarded.size() / bytesPerFrame);
        if (writeSpatialSilence(discardedFrames, QStringLiteral("spatial-static-bed-warmup"))) {
            m_pendingRecoveryWarmupFrames = discardedFrames >= m_pendingRecoveryWarmupFrames
                ? 0
                : m_pendingRecoveryWarmupFrames - discardedFrames;
        }
        return;
    }

    const UINT32 framesToWrite = qMin(frameCountPerBuffer, bufferedFrames);
    if (framesToWrite == 0) {
        if (!endOfStream) {
            ++m_waitForDataStreak;
            if (m_waitForDataStreak == 1 || m_waitForDataStreak == 4 || m_waitForDataStreak == 12) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("spatialStaticBed wait-for-data session=%1 streak=%2 frameCountPerBuffer=%3 bufferedFrames=%4")
                                      .arg(m_sessionId)
                                      .arg(m_waitForDataStreak)
                                      .arg(frameCountPerBuffer)
                                      .arg(bufferedFrames));
            }
        }
        return;
    }

    QByteArray chunk = m_buffer->readForOwner(static_cast<qint64>(framesToWrite) * bytesPerFrame,
                                             m_sessionId,
                                             m_outputBufferGeneration);
    const qsizetype alignedBytes = chunk.size() - (chunk.size() % bytesPerFrame);
    chunk.truncate(alignedBytes);
    guardActiveSwitchFirstDataBlockFade(chunk);
    PcmFadeApplication fade = applyPcmFadeIn(chunk);
    applyOutputVolume(chunk);
    const UINT32 writtenFrames = static_cast<UINT32>(chunk.size() / bytesPerFrame);
    if (writtenFrames == 0 || !writeSpatialPcm(chunk, writtenFrames)) {
        return;
    }

    m_submittedFrames += writtenFrames;
    m_waitForDataStreak = 0;
    const bool needMetrics = artifactTrackingEnabled()
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart");
    const RenderedBlockMetrics submittedMetrics =
        needMetrics ? renderedBlockMetricsForChunk(chunk, m_bufferFormat) : RenderedBlockMetrics{};
    noteFirstSubmittedPcmAfterSeek(writtenFrames,
                                   0,
                                   frameCountPerBuffer,
                                   submittedMetrics,
                                   fade);
    mirrorSubmittedBlock(chunk,
                         writtenFrames,
                         0,
                         frameCountPerBuffer,
                         QStringLiteral("spatial-static-bed-render"),
                         false,
                         false,
                         m_firstDataBlockAfterConfigure);
    analyzeArtifactBlock(chunk,
                         writtenFrames,
                         0,
                         frameCountPerBuffer,
                         QStringLiteral("spatial-static-bed-render"));
    if (m_pendingStartupSilenceFrames == 0 && m_pendingRecoveryWarmupFrames == 0) {
        scheduleFadeInStreamGain();
    }
}
