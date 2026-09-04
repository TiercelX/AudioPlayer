#include "windowswasapiaudioplayer_worker.h"

#include <cmath>

namespace {

constexpr int kSpatialFlushBufferMarginMs = 80;
constexpr REFERENCE_TIME kReferenceTimePerMs = 10000;
constexpr qint64 kSubmitBudgetMinMs = 64;
constexpr qint64 kSubmitBudgetMaxMs = 500;

} // namespace

bool WasapiOutputWorker::flushSpatialEndpoint(const QAudioDevice &device,
                                              const PcmStreamFormat &deviceFormat,
                                              const QByteArray &waveFormatData,
                                              int flushMs,
                                              int settleMs,
                                              const QString &reason,
                                              const QString &sourcePath,
                                              const QString &previousSourcePath,
                                              const QString &startupProfile,
                                              const QString &appStartTimeUtc)
{
    const int boundedFlushMs = qBound(1, flushMs, 1000);
    const int boundedSettleMs = qBound(0, settleMs, 1000);
    const QString deviceId = QString::fromLatin1(device.id().toHex());
    const auto fail = [&](const QString &stage, HRESULT hr) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialEndpointFlushFailed reason=%1 stage=%2 hr=0x%3 deviceId=%4 startupProfile=%5 spatialAudioMode=unknown")
                              .arg(reason,
                                   stage,
                                   QString::number(static_cast<qulonglong>(hr), 16),
                                   deviceId,
                                   startupProfile));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("spatial_endpoint_flush_failed"),
                                 {
                                     {QStringLiteral("spatialEndpointFlushEnabled"), true},
                                     {QStringLiteral("reason"), reason},
                                     {QStringLiteral("stage"), stage},
                                     {QStringLiteral("hresult"), QStringLiteral("0x%1").arg(QString::number(static_cast<qulonglong>(hr), 16))},
                                     {QStringLiteral("sourcePath"), sourcePath},
                                     {QStringLiteral("previousSourcePath"), previousSourcePath},
                                     {QStringLiteral("selectedOutputDeviceId"), deviceId},
                                     {QStringLiteral("startupProfile"), startupProfile},
                                     {QStringLiteral("appStartTime"), appStartTimeUtc},
                                     {QStringLiteral("spatialAudioMode"), QStringLiteral("unknown")},
                                 });
        return false;
    };

    if (!deviceFormat.isValid() || waveFormatData.size() < static_cast<int>(sizeof(WAVEFORMATEX))) {
        return fail(QStringLiteral("invalid-format"), E_INVALIDARG);
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialEndpointFlushBegin reason=%1 flushMs=%2 settleMs=%3 deviceId=%4 rate=%5 channels=%6 bits=%7 startupProfile=%8 spatialAudioMode=unknown")
                          .arg(reason)
                          .arg(boundedFlushMs)
                          .arg(boundedSettleMs)
                          .arg(deviceId)
                          .arg(deviceFormat.sampleRate)
                          .arg(deviceFormat.channelCount)
                          .arg(deviceFormat.bitsPerSample())
                          .arg(startupProfile));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("spatial_endpoint_flush_begin"),
                             {
                                 {QStringLiteral("spatialEndpointFlushEnabled"), true},
                                 {QStringLiteral("spatialEndpointFlushMs"), boundedFlushMs},
                                 {QStringLiteral("spatialEndpointSettleMs"), boundedSettleMs},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("sourcePath"), sourcePath},
                                 {QStringLiteral("previousSourcePath"), previousSourcePath},
                                 {QStringLiteral("selectedOutputDeviceId"), deviceId},
                                 {QStringLiteral("sampleRate"), deviceFormat.sampleRate},
                                 {QStringLiteral("channelCount"), deviceFormat.channelCount},
                                 {QStringLiteral("bitsPerSample"), deviceFormat.bitsPerSample()},
                                 {QStringLiteral("validBitsPerSample"), deviceFormat.effectiveValidBitsPerSample()},
                                 {QStringLiteral("startupProfile"), startupProfile},
                                 {QStringLiteral("appStartTime"), appStartTimeUtc},
                                 {QStringLiteral("spatialAudioMode"), QStringLiteral("unknown")},
                             });

    HRESULT hr = ensureComInitialized();
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return fail(QStringLiteral("com-init"), hr);
    }

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                          nullptr,
                          CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr)) {
        return fail(QStringLiteral("create-enumerator"), hr);
    }

    IMMDevice *flushDevice = nullptr;
    const QString endpointId = endpointIdFromQtId(device.id());
    hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(endpointId.utf16()), &flushDevice);
    safeRelease(enumerator);
    if (FAILED(hr)) {
        return fail(QStringLiteral("open-endpoint"), hr);
    }

    IAudioClient *flushClient = nullptr;
    hr = flushDevice->Activate(__uuidof(IAudioClient),
                               CLSCTX_ALL,
                               nullptr,
                               reinterpret_cast<void **>(&flushClient));
    if (FAILED(hr)) {
        safeRelease(flushDevice);
        return fail(QStringLiteral("activate-audio-client"), hr);
    }

    const auto *waveFormat = reinterpret_cast<const WAVEFORMATEX *>(waveFormatData.constData());
    const REFERENCE_TIME bufferDuration = static_cast<REFERENCE_TIME>(boundedFlushMs + kSpatialFlushBufferMarginMs) * kReferenceTimePerMs;
    hr = flushClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                 0,
                                 bufferDuration,
                                 0,
                                 waveFormat,
                                 nullptr);
    if (FAILED(hr)) {
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("initialize"), hr);
    }

    IAudioStreamVolume *streamVolume = nullptr;
    hr = flushClient->GetService(__uuidof(IAudioStreamVolume),
                                 reinterpret_cast<void **>(&streamVolume));
    if (SUCCEEDED(hr) && streamVolume && deviceFormat.channelCount > 0) {
        QVarLengthArray<float, 8> gains(deviceFormat.channelCount);
        for (int channel = 0; channel < gains.size(); ++channel) {
            gains[channel] = 0.0f;
        }
        streamVolume->SetAllVolumes(static_cast<UINT32>(gains.size()), gains.data());
    }

    IAudioRenderClient *flushRenderClient = nullptr;
    hr = flushClient->GetService(__uuidof(IAudioRenderClient),
                                 reinterpret_cast<void **>(&flushRenderClient));
    if (FAILED(hr)) {
        safeRelease(streamVolume);
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("get-render-client"), hr);
    }

    UINT32 bufferFrameCount = 0;
    hr = flushClient->GetBufferSize(&bufferFrameCount);
    if (FAILED(hr) || bufferFrameCount == 0) {
        safeRelease(flushRenderClient);
        safeRelease(streamVolume);
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("get-buffer-size"), FAILED(hr) ? hr : E_FAIL);
    }

    const UINT32 requestedFrames = qMax<UINT32>(
        UINT32(1),
        static_cast<UINT32>(static_cast<quint64>(deviceFormat.sampleRate) * boundedFlushMs / 1000));
    const UINT32 framesToSubmit = qMin(bufferFrameCount, requestedFrames);
    BYTE *silentData = nullptr;
    hr = flushRenderClient->GetBuffer(framesToSubmit, &silentData);
    if (FAILED(hr)) {
        safeRelease(flushRenderClient);
        safeRelease(streamVolume);
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("get-buffer"), hr);
    }

    hr = flushRenderClient->ReleaseBuffer(framesToSubmit, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        safeRelease(flushRenderClient);
        safeRelease(streamVolume);
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("release-silence"), hr);
    }

    const int submittedMs =
        qMax(1, static_cast<int>(std::ceil(static_cast<double>(framesToSubmit) * 1000.0
                                           / static_cast<double>(deviceFormat.sampleRate))));
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialEndpointFlushSubmittedSilence reason=%1 frames=%2 requestedFrames=%3 submittedMs=%4 bufferFrames=%5 deviceId=%6")
                          .arg(reason)
                          .arg(framesToSubmit)
                          .arg(requestedFrames)
                          .arg(submittedMs)
                          .arg(bufferFrameCount)
                          .arg(deviceId));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("spatial_endpoint_flush_submitted_silence"),
                             {
                                 {QStringLiteral("spatialEndpointFlushEnabled"), true},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("submittedFrames"), static_cast<qint64>(framesToSubmit)},
                                 {QStringLiteral("requestedFrames"), static_cast<qint64>(requestedFrames)},
                                 {QStringLiteral("submittedMs"), submittedMs},
                                 {QStringLiteral("bufferFrames"), static_cast<qint64>(bufferFrameCount)},
                                 {QStringLiteral("selectedOutputDeviceId"), deviceId},
                                 {QStringLiteral("startupProfile"), startupProfile},
                                 {QStringLiteral("spatialAudioMode"), QStringLiteral("unknown")},
                             });

    hr = flushClient->Start();
    if (FAILED(hr)) {
        safeRelease(flushRenderClient);
        safeRelease(streamVolume);
        safeRelease(flushClient);
        safeRelease(flushDevice);
        return fail(QStringLiteral("start"), hr);
    }

    QThread::msleep(static_cast<unsigned long>(qMax(boundedFlushMs, submittedMs)));
    flushClient->Stop();
    flushClient->Reset();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialEndpointFlushStopReset reason=%1 deviceId=%2")
                          .arg(reason, deviceId));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("spatial_endpoint_flush_stop_reset"),
                             {
                                 {QStringLiteral("spatialEndpointFlushEnabled"), true},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("selectedOutputDeviceId"), deviceId},
                                 {QStringLiteral("startupProfile"), startupProfile},
                                 {QStringLiteral("spatialAudioMode"), QStringLiteral("unknown")},
                             });

    safeRelease(flushRenderClient);
    safeRelease(streamVolume);
    safeRelease(flushClient);
    safeRelease(flushDevice);

    if (boundedSettleMs > 0) {
        QThread::msleep(static_cast<unsigned long>(boundedSettleMs));
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("spatialEndpointFlushDone reason=%1 flushMs=%2 settleMs=%3 submittedFrames=%4 deviceId=%5 startupProfile=%6 spatialAudioMode=unknown")
                          .arg(reason)
                          .arg(boundedFlushMs)
                          .arg(boundedSettleMs)
                          .arg(framesToSubmit)
                          .arg(deviceId)
                          .arg(startupProfile));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("spatial_endpoint_flush_done"),
                             {
                                 {QStringLiteral("spatialEndpointFlushEnabled"), true},
                                 {QStringLiteral("spatialEndpointFlushMs"), boundedFlushMs},
                                 {QStringLiteral("spatialEndpointSettleMs"), boundedSettleMs},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("submittedFrames"), static_cast<qint64>(framesToSubmit)},
                                 {QStringLiteral("selectedOutputDeviceId"), deviceId},
                                 {QStringLiteral("startupProfile"), startupProfile},
                                 {QStringLiteral("spatialAudioMode"), QStringLiteral("unknown")},
                             });
    return true;
}

bool WasapiOutputWorker::submitPcmFadeOutBeforeStop()
{
    if (!m_audioClient || !m_renderClient || !m_buffer || !m_started
        || !m_deviceFormat.isValid() || !m_bufferFormat.isValid()
        || m_deviceFormat.sampleRate <= 0 || m_bufferFrameCount == 0) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("stopPcmFadeOut skipped session=%1 audioClient=%2 renderClient=%3 buffer=%4 started=%5 sampleRate=%6 bufferFrames=%7")
                              .arg(m_sessionId)
                              .arg(m_audioClient ? 1 : 0)
                              .arg(m_renderClient ? 1 : 0)
                              .arg(m_buffer ? 1 : 0)
                              .arg(m_started)
                              .arg(m_deviceFormat.sampleRate)
                              .arg(m_bufferFrameCount));
        return false;
    }

    const int bufferBytesPerFrame = m_bufferFormat.bytesPerFrame();
    const int renderBytesPerFrame = m_deviceFormat.bytesPerFrame();
    if (bufferBytesPerFrame <= 0 || renderBytesPerFrame <= 0) {
        return false;
    }

    const UINT32 fadeFrames = qMax<UINT32>(
        1,
        static_cast<UINT32>(m_deviceFormat.sampleRate * kPcmFadeOutDurationMs / 1000));
    const qint64 endpointBufferMs = qMax<qint64>(
        1,
        static_cast<qint64>(std::ceil(static_cast<double>(m_bufferFrameCount) * 1000.0
                                      / static_cast<double>(m_deviceFormat.sampleRate))));
    const qint64 submitBudgetMs = qBound<qint64>(
        kSubmitBudgetMinMs,
        endpointBufferMs * 2 + kPcmFadeOutDurationMs + 32,
        kSubmitBudgetMaxMs);
    QElapsedTimer submitTimer;
    submitTimer.start();
    UINT32 submittedFadeFrames = 0;
    UINT32 finalPaddingFrames = 0;
    bool drainCompleted = false;
    double maxSubmittedPeak = 0.0;
    double lastSubmittedSample = 0.0;
    QString abortReason;
    HRESULT abortHr = S_OK;

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("stopPcmFadeOut begin session=%1 fadeFrames=%2 durationMs=%3 endpointBufferFrames=%4 endpointBufferMs=%5 exclusiveMode=%6 bufferedBytes=%7")
                          .arg(m_sessionId)
                          .arg(fadeFrames)
                          .arg(kPcmFadeOutDurationMs)
                          .arg(m_bufferFrameCount)
                          .arg(endpointBufferMs)
                          .arg(m_exclusiveModeActive ? 1 : 0)
                          .arg(m_buffer->bufferedBytes()));

    while (submittedFadeFrames < fadeFrames && submitTimer.elapsed() < submitBudgetMs) {
        UINT32 packetFrames = 0;
        UINT32 paddingFrames = 0;
        if (m_exclusiveModeActive) {
            const HRESULT paddingHr = m_audioClient->GetCurrentPadding(&paddingFrames);
            if (FAILED(paddingHr)) {
                abortReason = QStringLiteral("exclusive-padding");
                abortHr = paddingHr;
                break;
            }
            if (paddingFrames > 0) {
                QThread::msleep(2);
                continue;
            }
            packetFrames = m_bufferFrameCount;
        } else {
            const HRESULT paddingHr = m_audioClient->GetCurrentPadding(&paddingFrames);
            if (FAILED(paddingHr)) {
                abortReason = QStringLiteral("shared-padding");
                abortHr = paddingHr;
                break;
            }
            packetFrames = m_bufferFrameCount > paddingFrames
                ? qMin(m_bufferFrameCount - paddingFrames, fadeFrames - submittedFadeFrames)
                : 0;
            if (packetFrames == 0) {
                QThread::msleep(2);
                continue;
            }
        }

        const UINT32 sourceFrames = qMin(packetFrames, fadeFrames - submittedFadeFrames);
        QByteArray chunk =
            m_buffer->readForOwner(static_cast<qint64>(sourceFrames) * bufferBytesPerFrame,
                                   m_sessionId,
                                   m_outputBufferGeneration);
        const qsizetype alignedBytes = chunk.size() - (chunk.size() % bufferBytesPerFrame);
        chunk.truncate(alignedBytes);
        const UINT32 writtenFrames = static_cast<UINT32>(chunk.size() / bufferBytesPerFrame);
        if (writtenFrames == 0) {
            abortReason = QStringLiteral("no-source-pcm");
            break;
        }

        applyStopPcmFadeOut(chunk, submittedFadeFrames, fadeFrames);

        BYTE *renderData = nullptr;
        const HRESULT getBufferHr = m_renderClient->GetBuffer(packetFrames, &renderData);
        if (FAILED(getBufferHr)) {
            abortReason = QStringLiteral("get-buffer");
            abortHr = getBufferHr;
            break;
        }

        const qsizetype renderBytes = static_cast<qsizetype>(packetFrames) * renderBytesPerFrame;
        std::memset(renderData, 0, static_cast<size_t>(renderBytes));
        if (!copyConvertedFramesToRenderBuffer(chunk, renderData)) {
            m_renderClient->ReleaseBuffer(packetFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            abortReason = QStringLiteral("copy-converted-frames");
            break;
        }

        const QByteArray submittedChunk(reinterpret_cast<const char *>(renderData), renderBytes);
        const RenderedBlockMetrics submittedMetrics =
            renderedBlockMetricsForChunk(submittedChunk, m_deviceFormat);
        const HRESULT releaseBufferHr = m_renderClient->ReleaseBuffer(packetFrames, 0);
        if (FAILED(releaseBufferHr)) {
            abortReason = QStringLiteral("release-buffer");
            abortHr = releaseBufferHr;
            break;
        }

        maxSubmittedPeak = qMax(maxSubmittedPeak, submittedMetrics.peak);
        lastSubmittedSample = submittedMetrics.lastSample;
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("stopPcmFadeOut block session=%1 packetFrames=%2 writtenFrames=%3 submittedFadeFramesBefore=%4 peak=%5 rms=%6 startSample=%7 endSample=%8")
                              .arg(m_sessionId)
                              .arg(packetFrames)
                              .arg(writtenFrames)
                              .arg(submittedFadeFrames)
                              .arg(metricText(submittedMetrics.peak))
                              .arg(metricText(submittedMetrics.rms))
                              .arg(fineMetricText(submittedMetrics.firstSample))
                              .arg(fineMetricText(submittedMetrics.lastSample)));
        mirrorSubmittedBlock(submittedChunk,
                             packetFrames,
                             paddingFrames,
                             packetFrames,
                             QStringLiteral("wasapi-stop-fade"),
                             false,
                             false,
                             false);
        submittedFadeFrames += writtenFrames;
        m_submittedFrames += packetFrames;
    }

    if (submittedFadeFrames >= fadeFrames) {
        QElapsedTimer drainTimer;
        drainTimer.start();
        const qint64 drainBudgetMs = qBound<qint64>(
            qint64(16),
            endpointBufferMs + kPcmFadeOutDurationMs + 32,
            qint64(240));
        while (drainTimer.elapsed() < drainBudgetMs) {
            const HRESULT paddingHr = m_audioClient->GetCurrentPadding(&finalPaddingFrames);
            if (FAILED(paddingHr)) {
                abortReason = QStringLiteral("drain-padding");
                abortHr = paddingHr;
                break;
            }
            if (finalPaddingFrames == 0) {
                drainCompleted = true;
                break;
            }
            QThread::msleep(2);
        }
    }

    const bool completed = submittedFadeFrames >= fadeFrames && drainCompleted;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("stopPcmFadeOut end session=%1 completed=%2 fadeFrames=%3 submittedFadeFrames=%4 finalGain=%5 maxSubmittedPeak=%6 lastSubmittedSample=%7 finalPaddingFrames=%8 drainCompleted=%9 elapsedMs=%10 exclusiveMode=%11 abortReason=%12 abortHr=0x%13")
                          .arg(m_sessionId)
                          .arg(completed ? 1 : 0)
                          .arg(fadeFrames)
                          .arg(submittedFadeFrames)
                          .arg(submittedFadeFrames >= fadeFrames ? QStringLiteral("0.000") : QStringLiteral("unknown"))
                          .arg(metricText(maxSubmittedPeak))
                          .arg(fineMetricText(lastSubmittedSample))
                          .arg(finalPaddingFrames)
                          .arg(drainCompleted ? 1 : 0)
                          .arg(submitTimer.elapsed())
                          .arg(m_exclusiveModeActive ? 1 : 0)
                          .arg(abortReason)
                          .arg(QString::number(static_cast<qulonglong>(abortHr), 16)));
    return completed;
}

void WasapiOutputWorker::drainMutedPaddingBeforeReset(UINT32 initialPaddingFrames)
{
    if (!m_audioClient || !m_started || !m_deviceFormat.isValid() || m_deviceFormat.sampleRate <= 0) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("drainBeforeReset skipped session=%1 initialPaddingFrames=%2 audioClient=%3 started=%4 sampleRate=%5")
                              .arg(m_sessionId)
                              .arg(initialPaddingFrames)
                              .arg(m_audioClient ? 1 : 0)
                              .arg(m_started)
                              .arg(m_deviceFormat.sampleRate));
        return;
    }

    UINT32 pendingFrames = initialPaddingFrames;
    if (pendingFrames == 0) {
        const HRESULT hr = m_audioClient->GetCurrentPadding(&pendingFrames);
        if (FAILED(hr) || pendingFrames == 0) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("drainBeforeReset skipped session=%1 initialPaddingFrames=%2 queriedPaddingFrames=%3 hr=0x%4")
                                  .arg(m_sessionId)
                                  .arg(initialPaddingFrames)
                                  .arg(pendingFrames)
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16)));
            return;
        }
    }

    const qint64 expectedDrainMs = qMax<qint64>(
        1,
        static_cast<qint64>(std::ceil(static_cast<double>(pendingFrames) * 1000.0
                                      / static_cast<double>(m_deviceFormat.sampleRate))));
    const qint64 drainBudgetMs = qBound<qint64>(qint64(16), expectedDrainMs + 16, qint64(100));
    QElapsedTimer timer;
    timer.start();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("drainBeforeReset begin session=%1 initialPaddingFrames=%2 pendingFrames=%3 expectedDrainMs=%4 budgetMs=%5")
                          .arg(m_sessionId)
                          .arg(initialPaddingFrames)
                          .arg(pendingFrames)
                          .arg(expectedDrainMs)
                          .arg(drainBudgetMs));

    UINT32 finalPaddingFrames = pendingFrames;
    while (timer.elapsed() < drainBudgetMs) {
        QThread::msleep(2);
        const HRESULT hr = m_audioClient->GetCurrentPadding(&finalPaddingFrames);
        if (FAILED(hr)) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("drainBeforeReset padding-query-failed session=%1 hr=0x%2 waitedMs=%3")
                                  .arg(m_sessionId)
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16))
                                  .arg(timer.elapsed()));
            return;
        }
        if (finalPaddingFrames == 0) {
            break;
        }
    }

    if (finalPaddingFrames > 0) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("output event=drain-before-reset-timeout session=%1 initialPaddingFrames=%2 finalPaddingFrames=%3 waitedMs=%4 budgetMs=%5")
                              .arg(m_sessionId)
                              .arg(pendingFrames)
                              .arg(finalPaddingFrames)
                              .arg(timer.elapsed())
                              .arg(drainBudgetMs));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("drain_before_reset_timeout"),
                                 {
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("initialPaddingFrames"), static_cast<qint64>(pendingFrames)},
                                     {QStringLiteral("finalPaddingFrames"), static_cast<qint64>(finalPaddingFrames)},
                                     {QStringLiteral("waitedMs"), timer.elapsed()},
                                     {QStringLiteral("budgetMs"), drainBudgetMs},
                                 });
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("drainBeforeReset done session=%1 initialPaddingFrames=%2 waitedMs=%3 budgetMs=%4")
                          .arg(m_sessionId)
                          .arg(pendingFrames)
                          .arg(timer.elapsed())
                          .arg(drainBudgetMs));
}

void WasapiOutputWorker::releaseOutput(int sessionId, bool immediateReset, bool submitStopPcmFade)
    {
        if (sessionId != 0 && sessionId != m_sessionId) {
            return;
        }

        const int releasedSessionId = m_sessionId;
        UINT32 paddingFrames = 0;
        if (m_audioClient) {
            m_audioClient->GetCurrentPadding(&paddingFrames);
        }
        const qint64 bufferedBytes = m_buffer ? m_buffer->bufferedBytes() : 0;
        if (releasedSessionId != 0 && (paddingFrames > 0 || bufferedBytes > 0)) {
            PlayerLogger::log(QStringLiteral("anomaly"),
                              QStringLiteral("output event=release-with-backlog session=%1 paddingFrames=%2 bufferedBytes=%3 immediateReset=%4 started=%5")
                                  .arg(releasedSessionId)
                                  .arg(paddingFrames)
                                  .arg(bufferedBytes)
                                  .arg(immediateReset)
                                  .arg(m_started));
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("releaseOutput begin requestSession=%1 currentSession=%2 immediateReset=%3 started=%4 paddingFrames=%5 bufferedBytes=%6 eventNotifier=%7 streamGain=%8")
                              .arg(sessionId)
                              .arg(releasedSessionId)
                              .arg(immediateReset)
                              .arg(m_started)
                              .arg(paddingFrames)
                              .arg(bufferedBytes)
                              .arg(m_eventNotifier ? 1 : 0)
                              .arg(QString::number(m_streamGain, 'f', 3)));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("release_output_begin"),
                                 {
                                     {QStringLiteral("sessionId"), releasedSessionId},
                                     {QStringLiteral("requestSessionId"), sessionId},
                                     {QStringLiteral("immediateReset"), immediateReset},
                                     {QStringLiteral("started"), m_started},
                                     {QStringLiteral("paddingFrames"), static_cast<qint64>(paddingFrames)},
                                     {QStringLiteral("bufferedBytes"), bufferedBytes},
                                     {QStringLiteral("eventNotifierPresent"), m_eventNotifier != nullptr},
                                     {QStringLiteral("streamGain"), m_streamGain},
                                 });
        m_positionTimer->stop();
        if (submitStopPcmFade) {
            submitPcmFadeOutBeforeStop();
        }
        finishRenderMirrorCapture(QStringLiteral("releaseOutput"));
        saveSubmittedTailFingerprint(QStringLiteral("releaseOutput"));
        fadeOutStreamGainBeforeStop();

        if (m_audioClient && m_started && m_deviceFormat.isValid() && m_deviceFormat.sampleRate > 0) {
            UINT32 paddingFramesAfterFade = 0;
            if (m_audioClient->GetCurrentPadding(&paddingFramesAfterFade) == S_OK && paddingFramesAfterFade > 0) {
                const qint64 paddingDrainMs = qBound<qint64>(
                    qint64(8),
                    static_cast<qint64>(std::ceil(static_cast<double>(paddingFramesAfterFade) * 1000.0
                                                  / static_cast<double>(m_deviceFormat.sampleRate))),
                    qint64(48));
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("postFadeDrain session=%1 paddingFrames=%2 drainMs=%3")
                                      .arg(releasedSessionId)
                                      .arg(paddingFramesAfterFade)
                                      .arg(paddingDrainMs));
                QThread::msleep(static_cast<unsigned long>(paddingDrainMs));
            }
        }

        if (m_eventNotifier) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("eventNotifier disabled session=%1 reason=releaseOutput callbacksDuringStopFade=%2")
                                  .arg(releasedSessionId)
                                  .arg(m_renderCallbacksDuringStopFade));
            m_eventNotifier->setEnabled(false);
            delete m_eventNotifier;
            m_eventNotifier = nullptr;
        }

        if (m_buffer) {
            disconnect(m_buffer, nullptr, this, nullptr);
            m_buffer = nullptr;
        }

        if (m_audioClient) {
            if (immediateReset) {
                drainMutedPaddingBeforeReset(paddingFrames);
            }
            UINT32 paddingBeforeStop = 0;
            if (m_audioClient->GetCurrentPadding(&paddingBeforeStop) == S_OK && paddingBeforeStop > 0) {
                PlayerLogger::log(QStringLiteral("anomaly"),
                                  QStringLiteral("output event=old-endpoint-padding-abandoned session=%1 paddingFrames=%2 streamGain=%3 immediateReset=%4")
                                      .arg(m_sessionId)
                                      .arg(paddingBeforeStop)
                                      .arg(QString::number(m_streamGain, 'f', 3))
                                      .arg(immediateReset));
                PlayerLogger::diagnostic(QStringLiteral("audio"),
                                         QStringLiteral("old_endpoint_padding_abandoned"),
                                         {
                                             {QStringLiteral("sessionId"), m_sessionId},
                                             {QStringLiteral("paddingFrames"), static_cast<qint64>(paddingBeforeStop)},
                                             {QStringLiteral("streamGain"), m_streamGain},
                                             {QStringLiteral("immediateReset"), immediateReset},
                                         });
            }
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("releaseOutput before-stop session=%1 paddingFrames=%2 immediateReset=%3 streamGain=%4")
                                  .arg(m_sessionId)
                                  .arg(paddingBeforeStop)
                                  .arg(immediateReset)
                                  .arg(QString::number(m_streamGain, 'f', 3)));
            const HRESULT stopHr = m_audioClient->Stop();
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("releaseOutput after-stop session=%1 hr=0x%2")
                                  .arg(m_sessionId)
                                  .arg(QString::number(static_cast<qulonglong>(stopHr), 16)));
            if (immediateReset) {
                UINT32 paddingBeforeReset = 0;
                m_audioClient->GetCurrentPadding(&paddingBeforeReset);
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("releaseOutput before-reset session=%1 paddingFrames=%2")
                                      .arg(m_sessionId)
                                      .arg(paddingBeforeReset));
                const HRESULT resetHr = m_audioClient->Reset();
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("releaseOutput after-reset session=%1 hr=0x%2")
                                      .arg(m_sessionId)
                                      .arg(QString::number(static_cast<qulonglong>(resetHr), 16)));
            }
        }

        if (m_refillEvent) {
            CloseHandle(m_refillEvent);
            m_refillEvent = nullptr;
        }

        safeRelease(m_streamVolume);
        safeRelease(m_renderClient);
        safeRelease(m_audioClient);
        releaseSpatialStaticBed();
        safeRelease(m_device);

        m_artifactMonitor.resetContinuity(QStringLiteral("releaseOutput"));
        m_artifactTracking = {};
        m_firstDataBlockAfterConfigure = false;
        m_bufferFrameCount = 0;
        m_submittedFrames = 0;
        m_pendingStartupSilenceFrames = 0;
        m_configuredStartupSilenceFrames = 0;
        m_configuredStartupSilenceMs = 0;
        m_pendingRecoveryWarmupFrames = 0;
        m_waitForDataStreak = 0;
        m_started = false;
        m_sessionId = 0;
        m_deviceFormat = {};
        m_bufferFormat = {};
        m_noiseShaperState.clear();
        m_waveFormatData.clear();
        m_pcmFadeTotalFrames = 0;
        m_pcmFadeFramesProcessed = 0;
        m_volumeRampTotalFrames = 0;
        m_volumeRampFramesProcessed = 0;
        m_volumeRampActive = false;
        m_idleSignaled = false;
        m_streamFadeInPending = false;
        m_streamFadeInScheduled = false;
        m_streamGain = 1.0f;
        m_stopFadeInProgress = false;
        m_renderCallbacksDuringStopFade = 0;
        m_terminalErrorLatched = false;
        m_previousRenderedBlock = {};
        m_activeSwitchEntryBridgeBlock = {};
        m_activeSwitchEntryBridgeFallback = false;
        m_submittedRenderTail.clear();
        m_submittedRenderTailFormat = {};
        m_seekResumeFirst50msSubmittedPcm.clear();
        m_seekResumeFirst50msTargetFrames = 0;
        m_seekResumeFirst50msCapturedFrames = 0;
        m_seekResumeFirst50msStartupSilenceFrames = 0;
        m_seekResumeFirst50msWarmupFrames = 0;
        m_seekResumeFirst50msRealPcmFrames = 0;
        m_seekResumeFirst50msLogged = false;
        m_outputBufferGeneration = 0;
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("releaseOutput done releasedSession=%1").arg(releasedSessionId));
        emit released(releasedSessionId);
    }

    void WasapiOutputWorker::setVolume(qreal volume)
    {
        const qreal clampedVolume = qBound<qreal>(0.0, volume, 1.0);
        if (qFuzzyCompare(m_targetVolume, clampedVolume)) {
            return;
        }

        if (m_bufferFormat.isValid() && m_started) {
            resetVolumeRamp(m_bufferFormat, m_currentVolume, clampedVolume);
        } else {
            m_currentVolume = clampedVolume;
            m_targetVolume = clampedVolume;
            m_volumeRampActive = false;
            m_volumeRampTotalFrames = 0;
            m_volumeRampFramesProcessed = 0;
        }

        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("setVolume session=%1 current=%2 target=%3 rampActive=%4")
                              .arg(m_sessionId)
                              .arg(QString::number(m_currentVolume, 'f', 3))
                              .arg(QString::number(clampedVolume, 'f', 3))
                              .arg(m_volumeRampActive));
    }

void WasapiOutputWorker::handleFatalError(HRESULT hr, bool openingStage)
    {
        if (m_terminalErrorLatched) {
            return;
        }

        m_terminalErrorLatched = true;
        if (m_eventNotifier) {
            m_eventNotifier->setEnabled(false);
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("eventNotifier disabled session=%1 reason=fatalError")
                                  .arg(m_sessionId));
        }
        m_positionTimer->stop();
        m_streamFadeInPending = false;
        m_streamFadeInScheduled = false;
        m_waitForDataStreak = 0;
        if (m_buffer) {
            disconnect(m_buffer, nullptr, this, nullptr);
        }
        if (m_audioClient && m_started) {
            m_audioClient->Stop();
        }
        if (m_spatialStream && m_started) {
            m_spatialStream->Stop();
        }
        m_started = false;
        m_streamGain = 0.0f;

        const auto error = mapWasapiError(hr, openingStage);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("wasapiError session=%1 openStage=%2 hr=0x%3 mappedError=%4 quiesced=1")
                              .arg(m_sessionId)
                              .arg(openingStage)
                              .arg(QString::number(static_cast<qulonglong>(hr), 16))
                              .arg(static_cast<int>(error)));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("sink_backend_error"),
                                 {
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("transactionKind"), m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery")
                                          ? QStringLiteral("recovery")
                                          : (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")
                                                 ? QStringLiteral("active-switch")
                                                 : QStringLiteral("none"))},
                                     {QStringLiteral("pipelineStartProfile"), m_artifactTracking.pipelineStartupProfile},
                                     {QStringLiteral("activeSwitchTrigger"), m_artifactTracking.activeSwitchTrigger},
                                     {QStringLiteral("activeSwitchPhase"), m_artifactTracking.activeSwitchPhase},
                                     {QStringLiteral("activeSwitchReason"), m_artifactTracking.activeSwitchReason},
                                     {QStringLiteral("openStage"), openingStage},
                                     {QStringLiteral("hresult"), QStringLiteral("0x%1").arg(QString::number(static_cast<qulonglong>(hr), 16))},
                                     {QStringLiteral("sinkErrorCode"), static_cast<int>(error)},
                                 });
        emit stateChanged(m_sessionId,
                          static_cast<int>(QAudio::StoppedState),
                          static_cast<int>(error));
    }

    qint64 WasapiOutputWorker::processedPositionMs()
    {
        if (m_terminalErrorLatched || (!m_audioClient && !m_spatialStream) || m_deviceFormat.sampleRate <= 0) {
            return 0;
        }

        if (m_spatialStream) {
            return static_cast<qint64>(m_submittedFrames * 1000
                                       / static_cast<quint64>(m_deviceFormat.sampleRate));
        }

        UINT32 paddingFrames = 0;
        const HRESULT hr = m_audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return 0;
        }

        const quint64 processedFrames = m_submittedFrames > paddingFrames
            ? m_submittedFrames - paddingFrames
            : 0;
        return static_cast<qint64>(processedFrames * 1000 / static_cast<quint64>(m_deviceFormat.sampleRate));
    }

    void WasapiOutputWorker::renderAvailableFrames()
    {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_lastRenderCallbackTime > 0 && m_deviceFormat.sampleRate > 0) {
            const qint64 elapsedMs = nowMs - m_lastRenderCallbackTime;
            constexpr qint64 kMinExpectedPeriodMs = 4;
            const qint64 expectedPeriodMs = qMax(kMinExpectedPeriodMs,
                static_cast<qint64>(m_bufferFrameCount) * 1000 / m_deviceFormat.sampleRate);
            constexpr qint64 kCallbackJitterToleranceMs = 2;
            if (elapsedMs > expectedPeriodMs + kCallbackJitterToleranceMs) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("render_callback_lag session=%1 elapsedMs=%2 expectedPeriodMs=%3 bufferFrameCount=%4 sampleRate=%5 pipelineStartProfile=%6")
                                      .arg(m_sessionId)
                                      .arg(elapsedMs)
                                      .arg(expectedPeriodMs)
                                      .arg(static_cast<qint64>(m_bufferFrameCount))
                                      .arg(static_cast<qint64>(m_deviceFormat.sampleRate))
                                      .arg(m_artifactTracking.pipelineStartupProfile));
            }
        }
        m_lastRenderCallbackTime = nowMs;

        if (m_spatialStream) {
            renderSpatialFrames();
            return;
        }

        if (m_stopFadeInProgress) {
            ++m_renderCallbacksDuringStopFade;
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("renderAvailableFrames during-stop-fade session=%1 count=%2 streamGain=%3")
                                  .arg(m_sessionId)
                                  .arg(m_renderCallbacksDuringStopFade)
                                  .arg(QString::number(m_streamGain, 'f', 3)));
        }
        if (m_terminalErrorLatched || !m_audioClient || !m_renderClient || !m_buffer || !m_deviceFormat.isValid()
            || !m_bufferFormat.isValid()) {
            return;
        }
        if (!m_buffer->matchesOwner(m_sessionId, m_outputBufferGeneration)) {
            bool staleRead = false;
            m_buffer->readForOwner(0, m_sessionId, m_outputBufferGeneration, &staleRead);
            return;
        }

        UINT32 paddingFrames = 0;
        HRESULT hr = m_audioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        const UINT32 availableFrames = m_bufferFrameCount > paddingFrames ? m_bufferFrameCount - paddingFrames : 0;
        if (availableFrames == 0) {
            emitIdleIfDrained();
            return;
        }

        const int renderBytesPerFrame = m_deviceFormat.bytesPerFrame();
        const int bufferBytesPerFrame = m_bufferFormat.bytesPerFrame();
        if (renderBytesPerFrame <= 0 || bufferBytesPerFrame <= 0) {
            return;
        }

        const UINT32 bufferedFrames = static_cast<UINT32>(m_buffer->bufferedBytes() / bufferBytesPerFrame);
        const bool endOfStream = m_buffer->endOfStream();
        if (bufferedFrames == 0 && endOfStream) {
            emitIdleIfDrained();
            return;
        }

        const UINT32 framesToWrite = qMin(availableFrames, bufferedFrames);
        if (framesToWrite == 0) {
            if (!endOfStream) {
                ++m_waitForDataStreak;
                if (m_started && (m_waitForDataStreak == 1 || m_waitForDataStreak == 4
                                  || m_waitForDataStreak == 12)
                    && paddingFrames <= m_bufferFrameCount / 4) {
                    const QString transactionKind =
                        m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")
                            ? QStringLiteral("active-switch")
                            : (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery")
                                   ? QStringLiteral("recovery")
                                   : QStringLiteral("none"));
                    PlayerLogger::log(QStringLiteral("anomaly"),
                                      QStringLiteral("output event=wait-for-data session=%1 streak=%2 availableFrames=%3 paddingFrames=%4 bufferedFrames=%5 startupSilence=%6 warmupFrames=%7")
                                          .arg(m_sessionId)
                                          .arg(m_waitForDataStreak)
                                          .arg(availableFrames)
                                          .arg(paddingFrames)
                                          .arg(bufferedFrames)
                                          .arg(m_pendingStartupSilenceFrames)
                                          .arg(m_pendingRecoveryWarmupFrames));
                    PlayerLogger::log(QStringLiteral("audio"),
                                      QStringLiteral("buffer_starvation session=%1 transactionKind=%2 pipelineStartProfile=%3 activeSwitchTrigger=%4 activeSwitchPhase=%5 activeSwitchReason=%6 streak=%7 availableFrames=%8 paddingFrames=%9 bufferedFrames=%10 startupSilenceFrames=%11 warmupFrames=%12")
                                          .arg(m_sessionId)
                                          .arg(transactionKind)
                                          .arg(m_artifactTracking.pipelineStartupProfile)
                                          .arg(m_artifactTracking.activeSwitchTrigger)
                                          .arg(m_artifactTracking.activeSwitchPhase)
                                          .arg(m_artifactTracking.activeSwitchReason)
                                          .arg(m_waitForDataStreak)
                                          .arg(static_cast<qint64>(availableFrames))
                                          .arg(static_cast<qint64>(paddingFrames))
                                          .arg(static_cast<qint64>(bufferedFrames))
                                          .arg(static_cast<qint64>(m_pendingStartupSilenceFrames))
                                          .arg(static_cast<qint64>(m_pendingRecoveryWarmupFrames)));
                }
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("renderAvailableFrames wait-for-data session=%1 availableFrames=%2 bufferedFrames=%3")
                                      .arg(m_sessionId)
                                      .arg(availableFrames)
                                      .arg(bufferedFrames));
            }
            emitIdleIfDrained();
            return;
        }

        if (m_pendingStartupSilenceFrames > 0) {
            const UINT32 silentFrames = m_exclusiveModeActive
                ? framesToWrite
                : qMin(framesToWrite, m_pendingStartupSilenceFrames);
            const bool activeSwitchEntryBridge =
                m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild");
            if (activeSwitchEntryBridge) {
                const bool supportedRenderConversion =
                    hasSamePcmLayout(m_bufferFormat, m_deviceFormat)
                    || (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
                        && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int24
                        && m_bufferFormat.channelCount == m_deviceFormat.channelCount)
                    || (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
                        && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int32
                        && m_bufferFormat.effectiveValidBitsPerSample() == 32
                        && m_deviceFormat.effectiveValidBitsPerSample() == 24
                        && m_bufferFormat.channelCount == m_deviceFormat.channelCount)
                    || (m_bufferFormat.sampleEncoding == PcmSampleEncoding::Int32
                        && m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int16
                        && m_bufferFormat.channelCount == m_deviceFormat.channelCount);
                QString fallbackReason;
                if (!supportedRenderConversion) {
                    fallbackReason = QStringLiteral("unsupported-render-conversion");
                } else {
                    QByteArray bridgeChunk =
                        m_buffer->readForOwner(static_cast<qint64>(silentFrames) * bufferBytesPerFrame,
                                               m_sessionId,
                                               m_outputBufferGeneration);
                    const qsizetype alignedBytes =
                        bridgeChunk.size() - (bridgeChunk.size() % bufferBytesPerFrame);
                    bridgeChunk.truncate(alignedBytes);
                    const UINT32 bridgeFrames =
                        static_cast<UINT32>(bridgeChunk.size() / bufferBytesPerFrame);

                    if (bridgeFrames == 0) {
                        fallbackReason = QStringLiteral("no-fresh-pcm");
                    } else {
                        BYTE *bridgeData = nullptr;
                        hr = m_renderClient->GetBuffer(bridgeFrames, &bridgeData);
                        if (FAILED(hr)) {
                            handleFatalError(hr, false);
                            return;
                        }

                        const qsizetype bridgeBytesToWrite =
                            static_cast<qsizetype>(bridgeFrames) * renderBytesPerFrame;
                        std::memset(bridgeData, 0, static_cast<size_t>(bridgeBytesToWrite));

                        const qsizetype guardedFadeFrames = static_cast<qsizetype>(
                            std::ceil(static_cast<double>(m_pcmFadeFramesProcessed + bridgeFrames + framesToWrite)
                                      / m_activeSwitchFirstBlockMaxFadeGain));
                        if (guardedFadeFrames > m_pcmFadeTotalFrames) {
                            PlayerLogger::log(QStringLiteral("audio"),
                                              QStringLiteral("activeSwitchFirstBlockEntryGuard session=%1 frames=%2 previousFadeFrames=%3 guardedFadeFrames=%4 maxFirstBlockGain=%5 scope=entryBridge")
                                                  .arg(m_sessionId)
                                                  .arg(bridgeFrames + framesToWrite)
                                                  .arg(m_pcmFadeTotalFrames)
                                                  .arg(guardedFadeFrames)
                                                  .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
                            m_pcmFadeTotalFrames = guardedFadeFrames;
                        }

                        applyPcmFadeIn(bridgeChunk);
                        const double bridgeEndFadeGain = currentFadeEndpointGain();
                        applyOutputVolume(bridgeChunk);
                        const bool copied = copyConvertedFramesToRenderBuffer(bridgeChunk, bridgeData);
                        const QByteArray submittedBridgeChunk =
                            copied ? QByteArray(reinterpret_cast<const char *>(bridgeData),
                                                static_cast<qsizetype>(bridgeFrames) * renderBytesPerFrame)
                                   : QByteArray();
                        const DWORD releaseFlags = copied ? 0 : AUDCLNT_BUFFERFLAGS_SILENT;
                        const float streamGainBeforeBridge = m_streamGain;
                        if (copied) {
                            setStreamGain(m_activeSwitchEntryBridgeStreamGain);
                        }
                        hr = m_renderClient->ReleaseBuffer(copied ? bridgeFrames : 0, releaseFlags);
                        if (FAILED(hr)) {
                            handleFatalError(hr, false);
                            return;
                        }

                        if (copied) {
                            const RenderedBlockMetrics bridgeMetrics =
                                artifactTrackingEnabled() ? renderedBlockMetricsForChunk(bridgeChunk)
                                                          : RenderedBlockMetrics{};
                            mirrorSubmittedBlock(submittedBridgeChunk,
                                                 bridgeFrames,
                                                 paddingFrames,
                                                 availableFrames,
                                                 QStringLiteral("wasapi-active-switch-entry-bridge"),
                                                 false,
                                                 false,
                                                 m_firstDataBlockAfterConfigure);
                            m_activeSwitchEntryBridgeBlock = bridgeMetrics;
                            m_activeSwitchEntryBridgeFallback = false;
                            m_pendingStartupSilenceFrames = bridgeFrames >= m_pendingStartupSilenceFrames
                                ? 0
                                : m_pendingStartupSilenceFrames - bridgeFrames;
                            m_submittedFrames += bridgeFrames;
                            m_waitForDataStreak = 0;
                            PlayerLogger::log(QStringLiteral("audio"),
                                              QStringLiteral("activeSwitchEntryBridge session=%1 frames=%2 requestedFrames=%3 remaining=%4 peak=%5 rms=%6 jump=%7 bridgeEndFadeGain=%8 fallbackToSilence=0 streamGainBeforeBridge=%9 streamGainAtBridge=%10 writeFrames=%11 wasapiPaddingFrames=%12 wasapiAvailableFrames=%13 pipelineStartProfile=%14 artifactPath=%15 activeSwitchTrigger=%16 activeSwitchPhase=%17 activeSwitchReason=%18 activeSwitchBoundaryPolicy=%19 firstBlockMaxFadeGain=%20")
                                                  .arg(m_sessionId)
                                                  .arg(bridgeFrames)
                                                  .arg(silentFrames)
                                                  .arg(m_pendingStartupSilenceFrames)
                                                  .arg(metricText(bridgeMetrics.peak))
                                                  .arg(metricText(bridgeMetrics.rms))
                                                  .arg(metricText(bridgeMetrics.jump))
                                                  .arg(metricText(bridgeEndFadeGain))
                                                  .arg(QString::number(streamGainBeforeBridge, 'f', 3))
                                                  .arg(QString::number(m_streamGain, 'f', 3))
                                                  .arg(bridgeFrames)
                                                  .arg(paddingFrames)
                                                  .arg(availableFrames)
                                                  .arg(m_artifactTracking.pipelineStartupProfile)
                                                  .arg(m_artifactTracking.artifactPath)
                                                  .arg(m_artifactTracking.activeSwitchTrigger)
                                                  .arg(m_artifactTracking.activeSwitchPhase)
                                                  .arg(m_artifactTracking.activeSwitchReason)
                                                  .arg(m_activeSwitchBoundaryPolicyName)
                                                  .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
                            analyzeArtifactBlock(bridgeChunk,
                                                 bridgeFrames,
                                                 paddingFrames,
                                                 availableFrames,
                                                 QStringLiteral("wasapi-active-switch-entry-bridge"));
                            return;
                        }

                        fallbackReason = QStringLiteral("copy-failed");
                    }
                }

                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("activeSwitchEntryBridge session=%1 frames=0 requestedFrames=%2 remaining=%3 peak=0.0000 rms=0.0000 jump=0.0000 bridgeEndFadeGain=%4 fallbackToSilence=1 fallbackReason=%5 streamGainBeforeBridge=%6 streamGainAtBridge=%7 writeFrames=0 wasapiPaddingFrames=%8 wasapiAvailableFrames=%9 pipelineStartProfile=%10 artifactPath=%11 activeSwitchTrigger=%12 activeSwitchPhase=%13 activeSwitchReason=%14 activeSwitchBoundaryPolicy=%15 firstBlockMaxFadeGain=%16")
                                      .arg(m_sessionId)
                                      .arg(silentFrames)
                                      .arg(m_pendingStartupSilenceFrames)
                                      .arg(metricText(currentFadeEndpointGain()))
                                      .arg(fallbackReason)
                                      .arg(QString::number(m_streamGain, 'f', 3))
                                      .arg(QString::number(m_streamGain, 'f', 3))
                                      .arg(paddingFrames)
                                      .arg(availableFrames)
                                      .arg(m_artifactTracking.pipelineStartupProfile)
                                      .arg(m_artifactTracking.artifactPath)
                                      .arg(m_artifactTracking.activeSwitchTrigger)
                                      .arg(m_artifactTracking.activeSwitchPhase)
                                      .arg(m_artifactTracking.activeSwitchReason)
                                      .arg(m_activeSwitchBoundaryPolicyName)
                                      .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
                m_activeSwitchEntryBridgeBlock = {};
                m_activeSwitchEntryBridgeFallback = true;
            }

            BYTE *silentData = nullptr;
            hr = m_renderClient->GetBuffer(silentFrames, &silentData);
            if (FAILED(hr)) {
                handleFatalError(hr, false);
                return;
            }

            hr = m_renderClient->ReleaseBuffer(silentFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) {
                handleFatalError(hr, false);
                return;
            }

            m_pendingStartupSilenceFrames = silentFrames >= m_pendingStartupSilenceFrames
                ? 0
                : m_pendingStartupSilenceFrames - silentFrames;
            m_submittedFrames += silentFrames;
            m_waitForDataStreak = 0;
            mirrorSubmittedBlock(QByteArray(static_cast<qsizetype>(silentFrames) * renderBytesPerFrame, '\0'),
                                 silentFrames,
                                 paddingFrames,
                                 availableFrames,
                                 QStringLiteral("wasapi-startup-silence"),
                                 false,
                                 true,
                                 false);
            observeArtifactSilence(silentFrames,
                                   paddingFrames,
                                   availableFrames,
                                   false,
                                   QStringLiteral("wasapi-startup-silence"));
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("startupSilence session=%1 frames=%2 remaining=%3")
                                  .arg(m_sessionId)
                                  .arg(silentFrames)
                                  .arg(m_pendingStartupSilenceFrames));
            return;
        }

        if (m_pendingRecoveryWarmupFrames > 0) {
            const UINT32 warmupFrames = m_exclusiveModeActive
                ? framesToWrite
                : qMin(framesToWrite, m_pendingRecoveryWarmupFrames);
            const QByteArray discardedChunk =
                m_buffer->readForOwner(static_cast<qint64>(warmupFrames) * bufferBytesPerFrame,
                                       m_sessionId,
                                       m_outputBufferGeneration);
            const UINT32 discardedFrames =
                static_cast<UINT32>(discardedChunk.size() / bufferBytesPerFrame);
            if (discardedFrames == 0) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("renderAvailableFrames wait-for-warmup-data session=%1 pendingFrames=%2 bufferedFrames=%3")
                                      .arg(m_sessionId)
                                      .arg(m_pendingRecoveryWarmupFrames)
                                      .arg(bufferedFrames));
                return;
            }

            BYTE *silentData = nullptr;
            hr = m_renderClient->GetBuffer(discardedFrames, &silentData);
            if (FAILED(hr)) {
                handleFatalError(hr, false);
                return;
            }

            hr = m_renderClient->ReleaseBuffer(discardedFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) {
                handleFatalError(hr, false);
                return;
            }

            m_pendingRecoveryWarmupFrames = discardedFrames >= m_pendingRecoveryWarmupFrames
                ? 0
                : m_pendingRecoveryWarmupFrames - discardedFrames;
            m_submittedFrames += discardedFrames;
            m_waitForDataStreak = 0;
            mirrorSubmittedBlock(QByteArray(static_cast<qsizetype>(discardedFrames) * renderBytesPerFrame, '\0'),
                                 discardedFrames,
                                 paddingFrames,
                                 availableFrames,
                                 QStringLiteral("wasapi-startup-warmup"),
                                 true,
                                 true,
                                 false);
            observeArtifactSilence(discardedFrames,
                                   paddingFrames,
                                   availableFrames,
                                   true,
                                   QStringLiteral("wasapi-startup-warmup"));
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("startupWarmup session=%1 frames=%2 remaining=%3")
                                  .arg(m_sessionId)
                                  .arg(discardedFrames)
                                  .arg(m_pendingRecoveryWarmupFrames));
            return;
        }

        BYTE *renderData = nullptr;
        hr = m_renderClient->GetBuffer(framesToWrite, &renderData);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        const qsizetype bytesToWrite = static_cast<qsizetype>(framesToWrite) * renderBytesPerFrame;
        std::memset(renderData, 0, static_cast<size_t>(bytesToWrite));

        QByteArray chunk;
        chunk = m_buffer->readForOwner(static_cast<qint64>(framesToWrite) * bufferBytesPerFrame,
                                       m_sessionId,
                                       m_outputBufferGeneration);
        const qsizetype alignedBytes = chunk.size() - (chunk.size() % bufferBytesPerFrame);
        chunk.truncate(alignedBytes);
        guardActiveSwitchFirstDataBlockFade(chunk);
        PcmFadeApplication fade;
        const UINT32 writtenFrames = static_cast<UINT32>(chunk.size() / bufferBytesPerFrame);
        const bool copied = writtenFrames > 0 && applyFadeVolumeAndConvert(chunk, renderData, fade);
        const QByteArray submittedChunk =
            copied ? QByteArray(reinterpret_cast<const char *>(renderData),
                                static_cast<qsizetype>(writtenFrames) * renderBytesPerFrame)
                   : QByteArray();

        DWORD releaseFlags = (!copied || writtenFrames == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
        hr = m_renderClient->ReleaseBuffer(copied ? writtenFrames : 0, releaseFlags);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        if (copied && writtenFrames > 0) {
            const bool needMetrics = artifactTrackingEnabled()
                || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
                || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart");
            const RenderedBlockMetrics submittedMetrics =
                needMetrics ? renderedBlockMetricsForChunk(submittedChunk, m_deviceFormat)
                            : RenderedBlockMetrics{};
            noteFirstSubmittedPcmAfterSeek(writtenFrames,
                                           paddingFrames,
                                           availableFrames,
                                           submittedMetrics,
                                           fade);
            mirrorSubmittedBlock(submittedChunk,
                                 writtenFrames,
                                 paddingFrames,
                                 availableFrames,
                                 QStringLiteral("wasapi-render"),
                                 false,
                                 false,
                                 m_firstDataBlockAfterConfigure);
            analyzeArtifactBlock(chunk,
                                 writtenFrames,
                                 paddingFrames,
                                 availableFrames,
                                 QStringLiteral("wasapi-render"));
        }
        m_submittedFrames += copied ? writtenFrames : 0;
        if (copied && writtenFrames > 0) {
            m_waitForDataStreak = 0;
        } else {
            PlayerLogger::log(QStringLiteral("anomaly"),
                              QStringLiteral("output event=silent-release session=%1 framesToWrite=%2 writtenFrames=%3 copied=%4 releaseFlags=%5")
                                  .arg(m_sessionId)
                                  .arg(framesToWrite)
                                  .arg(writtenFrames)
                                  .arg(copied)
                                  .arg(releaseFlags));
        }
        if (copied && writtenFrames > 0 && m_pendingStartupSilenceFrames == 0
            && m_pendingRecoveryWarmupFrames == 0) {
            scheduleFadeInStreamGain();
        }
        if (copied && writtenFrames > 0 && m_submittedFrames > 0
            && m_deviceFormat.sampleRate > 0) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_rateWindowStartTimeMs == 0) {
                m_rateWindowStartTimeMs = nowMs;
                m_rateWindowStartFrames = m_submittedFrames;
                m_rateLastLogTimeMs = nowMs;
            }
            const qint64 elapsedMs = nowMs - m_rateWindowStartTimeMs;
            if (elapsedMs >= 1900) {
                const quint64 submittedInWindow = m_submittedFrames - m_rateWindowStartFrames;
                const qint64 expectedFrames =
                    static_cast<qint64>(m_deviceFormat.sampleRate) * elapsedMs / 1000;
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("render_rate session=%1 pipelineStartProfile=%2 elapsedMs=%3 submittedFrames=%4 expectedFrames=%5 sampleRate=%6 totalSubmittedFrames=%7")
                                      .arg(m_sessionId)
                                      .arg(m_artifactTracking.pipelineStartupProfile)
                                      .arg(elapsedMs)
                                      .arg(static_cast<qint64>(submittedInWindow))
                                      .arg(expectedFrames)
                                      .arg(m_deviceFormat.sampleRate)
                                      .arg(static_cast<qint64>(m_submittedFrames)));
                m_rateWindowStartTimeMs = nowMs;
                m_rateWindowStartFrames = m_submittedFrames;
            }
            if (m_buffer && m_rateLastLogTimeMs > 0 && nowMs - m_rateLastLogTimeMs >= 500) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("render_buffer_level session=%1 pipelineStartProfile=%2 bufferedBytes=%3 writableBytes=%4 wasapiPaddingFrames=%5 bufferFrameCount=%6 endOfStream=%7")
                                      .arg(m_sessionId)
                                      .arg(m_artifactTracking.pipelineStartupProfile)
                                      .arg(static_cast<qint64>(m_buffer->bufferedBytes()))
                                      .arg(static_cast<qint64>(m_buffer->writableBytes()))
                                      .arg(static_cast<qint64>(paddingFrames))
                                      .arg(static_cast<qint64>(m_bufferFrameCount))
                                      .arg(m_buffer->endOfStream() ? 1 : 0));
                m_rateLastLogTimeMs = nowMs;
            }
        }
        emitIdleIfDrained();
    }

    void WasapiOutputWorker::configureOutput(int sessionId,
                         const QAudioDevice &device,
                         const PcmStreamFormat &deviceFormat,
                         const PcmStreamFormat &bufferFormat,
                         const QByteArray &waveFormatData,
                         qsizetype bufferSizeBytes,
                         qreal volume,
                         bool startMutedForFadeIn,
                         bool injectStartupSilence,
                         bool discardWarmupFrames,
                         UINT32 startupSilenceMsOverride,
                         UINT32 warmupDiscardMsOverride,
                         int pcmFadeInDurationMsOverride,
                         int streamFadeInDelayMs,
                         ActiveSwitchBoundaryPolicy activeSwitchBoundaryPolicy,
                         WasapiArtifactTrackingConfig artifactTracking,
                         bool exclusiveMode,
                         bool stabilityMode,
                         bool spatialStaticBedRequested)
    {
        const UINT32 startupSilenceMs = startupSilenceMsOverride > 0
            ? startupSilenceMsOverride
            : (discardWarmupFrames ? kRecoveryStartupSilenceMs : kHotReconfigureStartupSilenceMs);
        const bool activeSwitchRebuildConfigure =
            artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild");
        if (!activeSwitchRebuildConfigure) {
            activeSwitchBoundaryPolicy = {};
            activeSwitchBoundaryPolicy.name = QStringLiteral("none");
        }
        const int pcmFadeInDurationMs = pcmFadeInDurationMsOverride > 0
            ? pcmFadeInDurationMsOverride
            : (activeSwitchRebuildConfigure
                   ? activeSwitchBoundaryPolicy.pcmFadeInDurationMs
                   : kPcmFadeInDurationMs);
        const UINT32 warmupDiscardMs = warmupDiscardMsOverride > 0
            ? warmupDiscardMsOverride
            : kRecoveryWarmupSilenceMs;
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("configureOutput session=%1 sampleRate=%2 channels=%3 deviceBits=%4 bufferBits=%5 bufferSize=%6 startMutedForFadeIn=%7 startupSilence=%8 warmupDiscard=%9 startupSilenceMs=%10 pipelineStartProfile=%11 artifactPath=%12 artifactTracking=%13 activeSwitchBoundaryPolicy=%14 firstBlockMaxFadeGain=%15 pcmFadeInMs=%16 warmupDiscardMs=%17 streamFadeInDelayMs=%18")
                              .arg(sessionId)
                              .arg(deviceFormat.sampleRate)
                              .arg(deviceFormat.channelCount)
                              .arg(deviceFormat.bitsPerSample())
                              .arg(bufferFormat.bitsPerSample())
                              .arg(bufferSizeBytes)
                              .arg(startMutedForFadeIn)
                              .arg(injectStartupSilence)
                              .arg(discardWarmupFrames)
                              .arg(startupSilenceMs)
                              .arg(artifactTracking.pipelineStartupProfile.isEmpty()
                                       ? QStringLiteral("none")
                                       : artifactTracking.pipelineStartupProfile)
                              .arg(artifactTracking.artifactPath.isEmpty()
                                       ? QStringLiteral("none")
                                       : artifactTracking.artifactPath)
                              .arg(artifactTracking.enabled)
                              .arg(activeSwitchBoundaryPolicy.name)
                              .arg(QString::number(activeSwitchBoundaryPolicy.firstBlockMaxFadeGain, 'f', 2))
                              .arg(pcmFadeInDurationMs)
                              .arg(discardWarmupFrames ? warmupDiscardMs : 0)
                              .arg(qMax(0, streamFadeInDelayMs)));
        if (activeSwitchRebuildConfigure) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=configureOutput-begin session=%1 bufferSize=%2 startupSilenceMs=%3 startMutedForFadeIn=%4 activeSwitchBoundaryPolicy=%5 firstBlockMaxFadeGain=%6 pcmFadeInMs=%7")
                                  .arg(sessionId)
                                  .arg(bufferSizeBytes)
                                  .arg(startupSilenceMs)
                                  .arg(startMutedForFadeIn)
                                  .arg(activeSwitchBoundaryPolicy.name)
                                  .arg(QString::number(activeSwitchBoundaryPolicy.firstBlockMaxFadeGain, 'f', 2))
                                  .arg(pcmFadeInDurationMs));
        }
        const RenderedBlockMetrics previousRenderedBlock = m_lastRenderedBlock;
        const QByteArray previousSubmittedTail = m_submittedRenderTail;
        const PcmStreamFormat previousSubmittedTailFormat = m_submittedRenderTailFormat;
        releaseOutput(0, true);
        if (activeSwitchRebuildConfigure) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=configureOutput-after-release session=%1 previousValid=%2 previousFrames=%3")
                                  .arg(sessionId)
                                  .arg(previousRenderedBlock.valid ? 1 : 0)
                                  .arg(previousRenderedBlock.frameCount));
        }
        m_previousRenderedBlock =
            activeSwitchRebuildConfigure ? previousRenderedBlock : RenderedBlockMetrics {};
        m_lastRenderedBlock = {};
        m_activeSwitchEntryBridgeBlock = {};
        m_activeSwitchEntryBridgeFallback = false;

        if (!deviceFormat.isValid() || !bufferFormat.isValid()) {
            emit stateChanged(sessionId,
                              static_cast<int>(QAudio::StoppedState),
                              static_cast<int>(QtAudio::OpenError));
            return;
        }

        m_sessionId = sessionId;
        m_deviceFormat = deviceFormat;
        m_bufferFormat = bufferFormat;
        m_noiseShaperState.resize(deviceFormat.channelCount > 0 ? deviceFormat.channelCount : 0);
        {
            uint32_t seed = QRandomGenerator::global()->generate();
            for (auto &state : m_noiseShaperState) {
                state = NoiseShaperState{};
                state.rng = seed;
                seed = seed * 1103515245u + 12345u;
            }
        }
        m_waveFormatData = waveFormatData;
        m_artifactTracking = std::move(artifactTracking);
        m_submittedRenderTail.clear();
        m_submittedRenderTailFormat = {};
        m_activeSwitchBoundaryPolicyName = activeSwitchBoundaryPolicy.name;
        m_activeSwitchFirstBlockMaxFadeGain = activeSwitchBoundaryPolicy.firstBlockMaxFadeGain;
        m_activeSwitchEntryBridgeStreamGain = activeSwitchBoundaryPolicy.entryBridgeStreamGain;
        m_firstDataBlockAfterConfigure = true;
        m_artifactMonitor.resetContinuity(
            m_artifactTracking.enabled
                ? QStringLiteral("configureOutput:%1").arg(m_artifactTracking.pipelineStartupProfile)
                : QStringLiteral("configureOutput:disabled"));
        startRenderMirrorCapture(previousSubmittedTail, previousSubmittedTailFormat);
        m_targetVolume = volume;
        m_currentVolume = startMutedForFadeIn ? 0.0 : volume;
        m_idleSignaled = false;
        m_waitForDataStreak = 0;
        m_rateWindowStartTimeMs = 0;
        m_rateWindowStartFrames = 0;
        m_rateLastLogTimeMs = 0;
        m_lastRenderCallbackTime = 0;
        m_exclusiveModeActive = false;
        m_streamFadeInPending = startMutedForFadeIn;
        m_terminalErrorLatched = false;
        m_pendingStartupSilenceFrames = startMutedForFadeIn && injectStartupSilence
            ? qMax<UINT32>(1, static_cast<UINT32>(deviceFormat.sampleRate * startupSilenceMs / 1000))
            : 0;
        m_configuredStartupSilenceFrames = m_pendingStartupSilenceFrames;
        m_configuredStartupSilenceMs = m_pendingStartupSilenceFrames > 0 ? startupSilenceMs : 0;
        m_pendingRecoveryWarmupFrames = startMutedForFadeIn && discardWarmupFrames
            ? qMax<UINT32>(1, static_cast<UINT32>(bufferFormat.sampleRate * warmupDiscardMs / 1000))
            : 0;
        m_configuredWarmupDiscardMs = m_pendingRecoveryWarmupFrames > 0 ? warmupDiscardMs : 0;
        m_streamFadeInDelayMs = qMax(0, streamFadeInDelayMs);
        m_seekResumeFirstDecodedPcmAfterSeekMs = -1;
        m_seekResumeFirstSubmittedPcmAfterSeekMs = -1;
        m_seekResumeLatencyLogged = false;
        const bool seekResumeTracking =
            artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
            || artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart");
        m_seekResumeFirst50msSubmittedPcm.clear();
        m_seekResumeFirst50msTargetFrames = seekResumeTracking
            ? qMax<qint64>(1, static_cast<qint64>(deviceFormat.sampleRate) * 50 / 1000)
            : 0;
        m_seekResumeFirst50msCapturedFrames = 0;
        m_seekResumeFirst50msStartupSilenceFrames = 0;
        m_seekResumeFirst50msWarmupFrames = 0;
        m_seekResumeFirst50msRealPcmFrames = 0;
        m_seekResumeFirst50msLogged = false;
        resetPcmFadeIn(bufferFormat, true, pcmFadeInDurationMs);
        resetVolumeRamp(bufferFormat, startMutedForFadeIn ? 0.0 : volume, volume);
        if (activeSwitchRebuildConfigure) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("activeSwitchRebuildTiming marker=configureOutput-ready session=%1 startupSilenceFrames=%2 pcmFadeFrames=%3 streamFadePending=%4 activeSwitchBoundaryPolicy=%5 firstBlockMaxFadeGain=%6 pcmFadeInMs=%7")
                                  .arg(sessionId)
                                  .arg(m_pendingStartupSilenceFrames)
                                  .arg(m_pcmFadeTotalFrames)
                                  .arg(m_streamFadeInPending)
                                  .arg(m_activeSwitchBoundaryPolicyName)
                                  .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2))
                                  .arg(m_pcmFadeInDurationMs));
        }

        HRESULT hr = ensureComInitialized();
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("configureOutput com-init-failed session=%1 hr=0x%2")
                                  .arg(sessionId)
                                  .arg(QString::number(static_cast<qulonglong>(hr), 16)));
            emit stateChanged(sessionId,
                              static_cast<int>(QAudio::StoppedState),
                              static_cast<int>(QtAudio::OpenError));
            return;
        }

        if (m_waveFormatData.isEmpty()) {
            if (m_deviceFormat.sampleEncoding == PcmSampleEncoding::Int24) {
                emit stateChanged(sessionId,
                                  static_cast<int>(QAudio::StoppedState),
                                  static_cast<int>(QtAudio::OpenError));
                return;
            }

            QAudioFormat approximateFormat;
            approximateFormat.setSampleRate(m_deviceFormat.sampleRate);
            approximateFormat.setChannelCount(m_deviceFormat.channelCount);
            approximateFormat.setSampleFormat(m_deviceFormat.qAudioSampleFormat());
            WAVEFORMATEXTENSIBLE fallbackWaveFormat = {};
            if (!buildWaveFormat(approximateFormat, &fallbackWaveFormat)) {
                emit stateChanged(sessionId,
                                  static_cast<int>(QAudio::StoppedState),
                                  static_cast<int>(QtAudio::OpenError));
                return;
            }

            m_waveFormatData =
                QByteArray(reinterpret_cast<const char *>(&fallbackWaveFormat),
                           static_cast<int>(sizeof(fallbackWaveFormat)));
        }
        const auto *waveFormat = reinterpret_cast<const WAVEFORMATEX *>(m_waveFormatData.constData());

        IMMDeviceEnumerator *enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                              nullptr,
                              CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr)) {
            handleFatalError(hr, true);
            return;
        }

        const QString endpointId = endpointIdFromQtId(device.id());
        hr = enumerator->GetDevice(reinterpret_cast<LPCWSTR>(endpointId.utf16()), &m_device);
        safeRelease(enumerator);
        if (FAILED(hr)) {
            handleFatalError(hr, true);
            return;
        }

        if (spatialStaticBedRequested && configureSpatialStaticBed(device)) {
            return;
        }
        if (spatialStaticBedRequested) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("spatialStaticBed fallback-to-wasapi session=%1 layout=%2")
                                  .arg(m_sessionId)
                                  .arg(m_bufferFormat.channelLayout));
        }

        hr = m_device->Activate(__uuidof(IAudioClient),
                                CLSCTX_ALL,
                                nullptr,
                                reinterpret_cast<void **>(&m_audioClient));
        if (FAILED(hr)) {
            handleFatalError(hr, true);
            return;
        }

        const REFERENCE_TIME sharedBufferDuration =
            exclusiveMode || !stabilityMode ? 0 : bufferDurationForBytes(bufferSizeBytes, m_deviceFormat);
        REFERENCE_TIME appliedSharedBufferDuration = sharedBufferDuration;

        if (exclusiveMode) {
            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           kExclusiveBufferDuration,
                                           kExclusiveBufferDuration,
                                           waveFormat,
                                           nullptr);
            if (FAILED(hr)) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("exclusiveMode initialize-fallback-shared hr=0x%1 rate=%2 channels=%3 bits=%4 validBits=%5")
                                      .arg(QString::number(static_cast<qulonglong>(hr), 16))
                                      .arg(m_deviceFormat.sampleRate)
                                      .arg(m_deviceFormat.channelCount)
                                      .arg(m_deviceFormat.bitsPerSample())
                                      .arg(m_deviceFormat.effectiveValidBitsPerSample()));
                hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                               sharedBufferDuration,
                                               0,
                                               waveFormat,
                                               nullptr);
                if (SUCCEEDED(hr)) {
                    m_exclusiveModeActive = false;
                }
            } else {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("exclusiveMode initialized session=%1 rate=%2 channels=%3 bits=%4 validBits=%5")
                                      .arg(m_sessionId)
                                      .arg(m_deviceFormat.sampleRate)
                                      .arg(m_deviceFormat.channelCount)
                                      .arg(m_deviceFormat.bitsPerSample())
                                      .arg(m_deviceFormat.effectiveValidBitsPerSample()));
                m_exclusiveModeActive = true;
            }
            if (FAILED(hr)) {
                handleFatalError(hr, true);
                return;
            }
        } else {
            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                           sharedBufferDuration,
                                           0,
                                           waveFormat,
                                           nullptr);
            if (FAILED(hr) && sharedBufferDuration > 0) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("wasapiBufferConfigured retry-default-shared-buffer session=%1 requestedDurationHns=%2 hr=0x%3")
                                      .arg(m_sessionId)
                                      .arg(static_cast<qint64>(sharedBufferDuration))
                                      .arg(QString::number(static_cast<qulonglong>(hr), 16)));
                hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                               0,
                                               0,
                                               waveFormat,
                                               nullptr);
                appliedSharedBufferDuration = 0;
            }
            if (FAILED(hr)) {
                handleFatalError(hr, true);
                return;
            }
            m_exclusiveModeActive = false;
        }
        hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("wasapiBufferConfigured session=%1 sharedBufferDurationHns=%2 requestedSharedBufferDurationHns=%3 bufferFrames=%4 exclusiveMode=%5 stabilityMode=%6")
                              .arg(m_sessionId)
                              .arg(static_cast<qint64>(appliedSharedBufferDuration))
                              .arg(static_cast<qint64>(sharedBufferDuration))
                              .arg(m_bufferFrameCount)
                              .arg(m_exclusiveModeActive ? 1 : 0)
                              .arg(stabilityMode ? 1 : 0));

        m_refillEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!m_refillEvent) {
            handleFatalError(HRESULT_FROM_WIN32(GetLastError()), false);
            return;
        }

        hr = m_audioClient->SetEventHandle(m_refillEvent);
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        hr = m_audioClient->GetService(__uuidof(IAudioRenderClient),
                                       reinterpret_cast<void **>(&m_renderClient));
        if (FAILED(hr)) {
            handleFatalError(hr, false);
            return;
        }

        hr = m_audioClient->GetService(__uuidof(IAudioStreamVolume),
                                       reinterpret_cast<void **>(&m_streamVolume));
        if (FAILED(hr)) {
            m_streamVolume = nullptr;
        }

        m_streamGain = startMutedForFadeIn ? 0.0f : 1.0f;
        setStreamGain(m_streamGain);

        m_eventNotifier = new QWinEventNotifier(m_refillEvent, this);
        m_eventNotifier->setEnabled(false);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("eventNotifier configured session=%1 enabled=0")
                              .arg(m_sessionId));
        connect(m_eventNotifier, &QWinEventNotifier::activated, this, [this](HANDLE) {
            if (m_stopFadeInProgress) {
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("eventNotifier activated-during-stop-fade session=%1 streamGain=%2")
                                      .arg(m_sessionId)
                                      .arg(QString::number(m_streamGain, 'f', 3)));
            }
            renderAvailableFrames();
        });
    }

void WasapiOutputWorker::pauseOutput(int sessionId)
{
    if ((!m_audioClient && !m_spatialStream) || !m_started || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("pauseOutput session=%1").arg(sessionId));
    const HRESULT hr = m_spatialStream ? m_spatialStream->Stop() : m_audioClient->Stop();
    if (FAILED(hr)) {
        handleFatalError(hr, false);
        return;
    }

    m_started = false;
    if (m_eventNotifier) {
        m_eventNotifier->setEnabled(false);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("eventNotifier disabled session=%1 reason=suspendOutput")
                              .arg(m_sessionId));
    }
    if (m_spatialEventNotifier) {
        m_spatialEventNotifier->setEnabled(false);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialEventNotifier disabled session=%1 reason=suspendOutput")
                              .arg(m_sessionId));
    }
    m_positionTimer->stop();
    emit positionUpdated(m_sessionId, processedPositionMs());
    emit stateChanged(m_sessionId,
                      static_cast<int>(QAudio::SuspendedState),
                      static_cast<int>(QtAudio::NoError));
}

void WasapiOutputWorker::resumeOutput(int sessionId)
{
    if ((!m_audioClient && !m_spatialStream) || m_started || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("resumeOutput session=%1").arg(sessionId));
    if (!m_spatialStream) {
        renderAvailableFrames();
    }
    const HRESULT hr = m_spatialStream ? m_spatialStream->Start() : m_audioClient->Start();
    if (FAILED(hr)) {
        handleFatalError(hr, false);
        return;
    }

    m_started = true;
    if (m_eventNotifier) {
        m_eventNotifier->setEnabled(true);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("eventNotifier enabled session=%1 reason=resumeOutput")
                              .arg(m_sessionId));
    }
    if (m_spatialEventNotifier) {
        m_spatialEventNotifier->setEnabled(true);
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("spatialEventNotifier enabled session=%1 reason=resumeOutput")
                              .arg(m_sessionId));
    }
    m_positionTimer->start();
    emit stateChanged(m_sessionId,
                      static_cast<int>(QAudio::ActiveState),
                      static_cast<int>(QtAudio::NoError));
}

void WasapiOutputWorker::prepareForOutputDeviceChange(int sessionId)
{
    if (!m_audioClient || !m_started || m_terminalErrorLatched || sessionId != m_sessionId) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("prepareForOutputDeviceChange session=%1 currentGain=%2")
                          .arg(sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3)));
    fadeOutStreamGainBeforeStop();
}

void WasapiOutputWorker::restoreAfterCancelledOutputDeviceChange(int sessionId)
{
    if (!m_audioClient || m_terminalErrorLatched || sessionId != m_sessionId || m_streamGain >= 0.999f) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("restoreAfterCancelledOutputDeviceChange session=%1 currentGain=%2")
                          .arg(sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3)));
    fadeStreamGainTo(1.0f);
}

void WasapiOutputWorker::restoreAfterCancelledActiveOutputInvalidation(int sessionId)
{
    if (!m_audioClient || m_terminalErrorLatched || sessionId != m_sessionId || m_streamGain >= 0.999f) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("restoreAfterCancelledActiveOutputInvalidation session=%1 currentGain=%2")
                          .arg(sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3)));
    fadeStreamGainTo(1.0f);
}

void WasapiOutputWorker::restoreActiveOutputInvalidationTaper(int sessionId)
{
    if (!m_audioClient || m_terminalErrorLatched || sessionId != m_sessionId || m_streamGain >= 0.999f) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("restoreActiveOutputInvalidationTaper session=%1 currentGain=%2")
                          .arg(sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3)));
    fadeStreamGainTo(1.0f);
}

HRESULT WasapiOutputWorker::ensureComInitialized()
{
    if (m_comInitialized) {
        return S_OK;
    }

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        m_shouldUninitializeCom = true;
        m_comInitialized = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        m_comInitialized = true;
    }
    return hr;
}

void WasapiOutputWorker::fadeOutStreamGainBeforeStop()
{
    if (!m_started || !m_streamVolume || m_streamGain <= 0.001f) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("fadeOutStreamGain skipped session=%1 started=%2 streamVolume=%3 streamGain=%4")
                              .arg(m_sessionId)
                              .arg(m_started)
                              .arg(m_streamVolume ? 1 : 0)
                              .arg(QString::number(m_streamGain, 'f', 3)));
        return;
    }

    m_stopFadeInProgress = true;
    m_renderCallbacksDuringStopFade = 0;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("fadeOutStreamGain begin session=%1 startGain=%2 eventNotifier=%3")
                          .arg(m_sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3))
                          .arg(m_eventNotifier ? 1 : 0));
    fadeStreamGainTo(0.0f, true);
    m_stopFadeInProgress = false;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("fadeOutStreamGain end session=%1 finalGain=%2 renderCallbacksDuringFade=%3")
                          .arg(m_sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3))
                          .arg(m_renderCallbacksDuringStopFade));
}

void WasapiOutputWorker::fadeInStreamGainIfNeeded()
{
    if (!m_streamFadeInPending) {
        return;
    }

    m_streamFadeInPending = false;
    m_streamFadeInScheduled = false;
    logSeekResumeLatencyIfNeeded(QDateTime::currentMSecsSinceEpoch());
    if (!m_streamVolume) {
        m_streamGain = 1.0f;
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("fadeInStreamGain session=%1")
                          .arg(m_sessionId));
    fadeStreamGainTo(1.0f);
}

void WasapiOutputWorker::scheduleFadeInStreamGain()
{
    if (!m_streamFadeInPending || m_streamFadeInScheduled || !m_started) {
        return;
    }

    m_streamFadeInScheduled = true;
    const int scheduledSessionId = m_sessionId;
    const int delayMs = qMax(0, m_streamFadeInDelayMs);
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("scheduleFadeInStreamGain session=%1 delayMs=%2")
                          .arg(scheduledSessionId)
                          .arg(delayMs));
    QTimer::singleShot(delayMs, this, [this, scheduledSessionId] {
        m_streamFadeInScheduled = false;
        if (scheduledSessionId != m_sessionId) {
            return;
        }

        fadeInStreamGainIfNeeded();
    });
}

void WasapiOutputWorker::prepareForActiveOutputInvalidation(int sessionId)
{
    if (!m_audioClient || !m_started || m_terminalErrorLatched || sessionId != m_sessionId
        || m_streamGain <= kActiveSwitchInvalidationTaperGain + 0.001f) {
        return;
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("prepareForActiveOutputInvalidation session=%1 currentGain=%2 targetGain=%3 holdMs=%4")
                          .arg(sessionId)
                          .arg(QString::number(m_streamGain, 'f', 3))
                          .arg(QString::number(kActiveSwitchInvalidationTaperGain, 'f', 3))
                          .arg(kActiveSwitchInvalidationTaperHoldMs));
    fadeStreamGainTo(kActiveSwitchInvalidationTaperGain, true);
    QTimer::singleShot(kActiveSwitchInvalidationTaperHoldMs, this, [this, sessionId] {
        restoreActiveOutputInvalidationTaper(sessionId);
    });
}
