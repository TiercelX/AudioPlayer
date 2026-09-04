#include "windowswasapiaudioplayer_worker.h"

QJsonObject WasapiOutputWorker::formatJson(const PcmStreamFormat &format) const
{
    QJsonObject object;
    object.insert(QStringLiteral("sampleRate"), format.sampleRate);
    object.insert(QStringLiteral("channelCount"), format.channelCount);
    object.insert(QStringLiteral("sampleEncoding"), pcmEncodingName(format.sampleEncoding));
    object.insert(QStringLiteral("bitsPerSample"), format.bitsPerSample());
    object.insert(QStringLiteral("validBitsPerSample"), format.effectiveValidBitsPerSample());
    object.insert(QStringLiteral("bytesPerFrame"), format.bytesPerFrame());
    object.insert(QStringLiteral("channelLayout"), format.channelLayout);
    return object;
}

QString WasapiOutputWorker::renderMirrorBasePath(int sessionId) const
{
    const QFileInfo logInfo(PlayerLogger::logFilePath());
    return QDir(logInfo.absolutePath()).filePath(
        QStringLiteral("%1-render-mirror-session%2").arg(logInfo.completeBaseName()).arg(sessionId));
}

QString WasapiOutputWorker::previousRunTailFingerprintPath() const
{
    const QFileInfo logInfo(PlayerLogger::logFilePath());
    return QDir(logInfo.absolutePath()).filePath(
        QStringLiteral("previous-run-submitted-tail-fingerprint.json"));
}

void WasapiOutputWorker::saveSubmittedTailFingerprint(const QString &reason)
{
    if (m_submittedRenderTail.isEmpty() || !m_submittedRenderTailFormat.isValid()) {
        return;
    }

    QJsonObject fingerprint;
    fingerprint.insert(QStringLiteral("sourcePath"), m_artifactTracking.source);
    fingerprint.insert(QStringLiteral("sessionId"), m_sessionId);
    fingerprint.insert(QStringLiteral("bytes"), m_submittedRenderTail.size());
    fingerprint.insert(QStringLiteral("tailWindowMs"), kSubmittedTailWindowMs);
    fingerprint.insert(QStringLiteral("sha256"),
                       QString::fromLatin1(QCryptographicHash::hash(m_submittedRenderTail,
                                                                      QCryptographicHash::Sha256)
                                                .toHex()));
    fingerprint.insert(QStringLiteral("format"), formatJson(m_submittedRenderTailFormat));
    fingerprint.insert(QStringLiteral("finishReason"), reason);
    fingerprint.insert(QStringLiteral("appStartTime"), m_artifactTracking.appStartTimeUtc);
    fingerprint.insert(QStringLiteral("writtenAt"),
                       QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

    const QString path = previousRunTailFingerprintPath();
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        file.write(QJsonDocument(fingerprint).toJson(QJsonDocument::Indented));
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("previousRunSubmittedTailFingerprint path=%1 bytes=%2 source=%3 session=%4 reason=%5")
                              .arg(path)
                              .arg(m_submittedRenderTail.size())
                              .arg(m_artifactTracking.source)
                              .arg(m_sessionId)
                              .arg(reason));
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("previous_run_submitted_tail_fingerprint"),
                                 {
                                     {QStringLiteral("path"), path},
                                     {QStringLiteral("bytes"), m_submittedRenderTail.size()},
                                     {QStringLiteral("tailWindowMs"), kSubmittedTailWindowMs},
                                     {QStringLiteral("sourcePath"), m_artifactTracking.source},
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("observationLayer"), QStringLiteral("WASAPI submitted PCM before endpoint output")},
                                 });
    }
}

void WasapiOutputWorker::startRenderMirrorCapture(const QByteArray &previousTail,
                                                   const PcmStreamFormat &previousTailFormat)
{
    finishRenderMirrorCapture(QStringLiteral("restart"));
    const bool mirrorContext =
        m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("NormalStart")
        || m_artifactTracking.artifactPath == QStringLiteral("SourceSwitch");
    if (!mirrorContext || !m_deviceFormat.isValid()) {
        return;
    }

    const QString basePath = renderMirrorBasePath(m_sessionId);
    m_renderMirrorRawPath = basePath + QStringLiteral("-post.raw");
    m_renderMirrorMetadataPath = basePath + QStringLiteral(".json");
    m_renderMirrorFile.setFileName(m_renderMirrorRawPath);
    if (!m_renderMirrorFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        PlayerLogger::log(QStringLiteral("anomaly"),
                          QStringLiteral("renderMirror start-failed session=%1 path=%2")
                              .arg(m_sessionId)
                              .arg(m_renderMirrorRawPath));
        m_renderMirrorRawPath.clear();
        m_renderMirrorMetadataPath.clear();
        return;
    }

    m_renderMirrorActive = true;
    m_renderMirrorCapturedFrames = 0;
    const int renderMirrorWindowMs =
        boundedEnvInt(kWasapiRenderMirrorWindowMsEnv, kRenderMirrorWindowMs, 100, 30000);
    m_renderMirrorMaxFrames =
        qMax<qint64>(1, static_cast<qint64>(m_deviceFormat.sampleRate) * renderMirrorWindowMs / 1000);
    m_renderMirrorMonitor.resetContinuity(QStringLiteral("render-mirror-start"));

    QJsonObject metadata;
    metadata.insert(QStringLiteral("sessionId"), m_sessionId);
    metadata.insert(QStringLiteral("source"), m_artifactTracking.source);
    metadata.insert(QStringLiteral("sourcePath"), m_artifactTracking.source);
    metadata.insert(QStringLiteral("previousSourcePath"), m_artifactTracking.previousSource);
    metadata.insert(QStringLiteral("startupProfile"), m_artifactTracking.pipelineStartupProfile);
    metadata.insert(QStringLiteral("startupObservationProfile"),
                    m_artifactTracking.startupObservationProfile);
    metadata.insert(QStringLiteral("artifactPath"), m_artifactTracking.artifactPath);
    metadata.insert(QStringLiteral("activeSwitchTrigger"), m_artifactTracking.activeSwitchTrigger);
    metadata.insert(QStringLiteral("phase"), m_artifactTracking.activeSwitchPhase);
    metadata.insert(QStringLiteral("reason"), m_artifactTracking.activeSwitchReason);
    metadata.insert(QStringLiteral("positionMs"), m_artifactTracking.startPositionMs);
    metadata.insert(QStringLiteral("bufferGeneration"),
                    static_cast<qint64>(m_artifactTracking.bufferGeneration));
    metadata.insert(QStringLiteral("selectedOutputDeviceId"), m_artifactTracking.selectedOutputDeviceId);
    metadata.insert(QStringLiteral("appStartTime"), m_artifactTracking.appStartTimeUtc);
    metadata.insert(QStringLiteral("outputFormat"), formatJson(m_deviceFormat));
    metadata.insert(QStringLiteral("postRawPath"), m_renderMirrorRawPath);
    metadata.insert(QStringLiteral("windowMs"), renderMirrorWindowMs);

    if (!previousTail.isEmpty() && previousTailFormat.isValid()) {
        const QString preRawPath = basePath + QStringLiteral("-pre.raw");
        QFile preFile(preRawPath);
        if (preFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            preFile.write(previousTail);
            metadata.insert(QStringLiteral("preRawPath"), preRawPath);
            metadata.insert(QStringLiteral("preBytes"), previousTail.size());
            metadata.insert(QStringLiteral("preFormat"), formatJson(previousTailFormat));
        }
    }

    m_renderMirrorMetadata = metadata;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("renderMirror start session=%1 startupProfile=%2 activeSwitchTrigger=%3 phase=%4 reason=%5 positionMs=%6 postRaw=%7 preBytes=%8")
                          .arg(m_sessionId)
                          .arg(m_artifactTracking.startupObservationProfile.isEmpty()
                                   ? m_artifactTracking.pipelineStartupProfile
                                   : m_artifactTracking.startupObservationProfile)
                          .arg(m_artifactTracking.activeSwitchTrigger)
                          .arg(m_artifactTracking.activeSwitchPhase)
                          .arg(m_artifactTracking.activeSwitchReason)
                          .arg(m_artifactTracking.startPositionMs)
                          .arg(m_renderMirrorRawPath)
                          .arg(previousTail.size()));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("render_mirror_start"),
                             {
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("positionMs"), m_artifactTracking.startPositionMs},
                                 {QStringLiteral("startupProfile"), m_artifactTracking.pipelineStartupProfile},
                                 {QStringLiteral("startupObservationProfile"), m_artifactTracking.startupObservationProfile},
                                 {QStringLiteral("activeSwitchTrigger"), m_artifactTracking.activeSwitchTrigger},
                                 {QStringLiteral("phase"), m_artifactTracking.activeSwitchPhase},
                                 {QStringLiteral("reason"), m_artifactTracking.activeSwitchReason},
                                 {QStringLiteral("sourcePath"), m_artifactTracking.source},
                                 {QStringLiteral("previousSourcePath"), m_artifactTracking.previousSource},
                                 {QStringLiteral("bufferGeneration"), static_cast<qint64>(m_artifactTracking.bufferGeneration)},
                                 {QStringLiteral("selectedOutputDeviceId"), m_artifactTracking.selectedOutputDeviceId},
                                 {QStringLiteral("appStartTime"), m_artifactTracking.appStartTimeUtc},
                                 {QStringLiteral("rawPath"), m_renderMirrorRawPath},
                                 {QStringLiteral("preBytes"), previousTail.size()},
                                 {QStringLiteral("windowMs"), renderMirrorWindowMs},
                                 {QStringLiteral("observationLayer"), QStringLiteral("WASAPI submitted PCM before endpoint output")},
                             });
    if (m_artifactTracking.startupObservationProfile == QStringLiteral("ColdStart")) {
        PlayerLogger::log(QStringLiteral("audio"),
                          QStringLiteral("coldStartMirrorObserved session=%1 source=%2 raw=%3 windowMs=%4")
                              .arg(m_sessionId)
                              .arg(m_artifactTracking.source)
                              .arg(m_renderMirrorRawPath)
                              .arg(renderMirrorWindowMs));
    }
}

void WasapiOutputWorker::finishRenderMirrorCapture(const QString &reason)
{
    if (!m_renderMirrorActive) {
        return;
    }

    if (m_renderMirrorFile.isOpen()) {
        m_renderMirrorFile.flush();
        m_renderMirrorFile.close();
    }

    const quint64 artifactCount = m_renderMirrorMonitor.artifactCountTotal();
    const bool artifactDetected = artifactCount > 0;
    const QString conclusion = artifactDetected
        ? QStringLiteral("submitted PCM artifact detected")
        : QStringLiteral("submitted PCM clean");
    m_renderMirrorMetadata.insert(QStringLiteral("capturedFrames"),
                                  static_cast<qint64>(m_renderMirrorCapturedFrames));
    m_renderMirrorMetadata.insert(QStringLiteral("capturedBytes"),
                                  QFileInfo(m_renderMirrorRawPath).size());
    m_renderMirrorMetadata.insert(QStringLiteral("artifactDetected"), artifactDetected);
    m_renderMirrorMetadata.insert(QStringLiteral("artifactCount"), static_cast<qint64>(artifactCount));
    m_renderMirrorMetadata.insert(QStringLiteral("conclusion"), conclusion);
    m_renderMirrorMetadata.insert(QStringLiteral("finishReason"), reason);
    if (m_renderMirrorMetadata.value(QStringLiteral("startupObservationProfile")).toString()
        == QStringLiteral("ColdStart")) {
        m_renderMirrorMetadata.insert(QStringLiteral("coldStartSubmittedMirrorClean"), !artifactDetected);
    }

    QFile metadataFile(m_renderMirrorMetadataPath);
    if (metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        metadataFile.write(QJsonDocument(m_renderMirrorMetadata).toJson(QJsonDocument::Indented));
    }

    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("renderMirrorConclusion session=%1 result=\"%2\" artifactDetected=%3 artifactCount=%4 frames=%5 raw=%6 metadata=%7 finishReason=%8 startupProfile=%9 artifactPath=%10 source=%11")
                          .arg(m_sessionId)
                          .arg(conclusion)
                          .arg(artifactDetected ? 1 : 0)
                          .arg(artifactCount)
                          .arg(m_renderMirrorCapturedFrames)
                          .arg(m_renderMirrorRawPath)
                          .arg(m_renderMirrorMetadataPath)
                          .arg(reason)
                          .arg(m_renderMirrorMetadata.value(QStringLiteral("startupObservationProfile")).toString().isEmpty()
                                   ? m_renderMirrorMetadata.value(QStringLiteral("startupProfile")).toString()
                                   : m_renderMirrorMetadata.value(QStringLiteral("startupObservationProfile")).toString())
                          .arg(m_renderMirrorMetadata.value(QStringLiteral("artifactPath")).toString())
                          .arg(m_renderMirrorMetadata.value(QStringLiteral("source")).toString()));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("render_mirror_conclusion"),
                             {
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("result"), conclusion},
                                 {QStringLiteral("source"), m_renderMirrorMetadata.value(QStringLiteral("source")).toString()},
                                 {QStringLiteral("startupProfile"), m_renderMirrorMetadata.value(QStringLiteral("startupProfile")).toString()},
                                 {QStringLiteral("startupObservationProfile"), m_renderMirrorMetadata.value(QStringLiteral("startupObservationProfile")).toString()},
                                 {QStringLiteral("artifactPath"), m_renderMirrorMetadata.value(QStringLiteral("artifactPath")).toString()},
                                 {QStringLiteral("artifactDetected"), artifactDetected},
                                 {QStringLiteral("artifactCount"), static_cast<qint64>(artifactCount)},
                                 {QStringLiteral("capturedFrames"), static_cast<qint64>(m_renderMirrorCapturedFrames)},
                                 {QStringLiteral("rawPath"), m_renderMirrorRawPath},
                                 {QStringLiteral("metadataPath"), m_renderMirrorMetadataPath},
                                 {QStringLiteral("observationLayer"), QStringLiteral("WASAPI submitted PCM before endpoint output")},
                             });

    m_renderMirrorActive = false;
    m_renderMirrorCapturedFrames = 0;
    m_renderMirrorMaxFrames = 0;
    m_renderMirrorRawPath.clear();
    m_renderMirrorMetadataPath.clear();
    m_renderMirrorMetadata = {};
}

void WasapiOutputWorker::appendSubmittedPcmTail(const QByteArray &submittedChunk, const PcmStreamFormat &format)
{
    if (submittedChunk.isEmpty() || !format.isValid()) {
        return;
    }

    m_submittedRenderTailFormat = format;
    m_submittedRenderTail.append(submittedChunk);
    const qint64 maxTailBytes =
        static_cast<qint64>(format.bytesPerFrame()) * format.sampleRate * kSubmittedTailWindowMs / 1000;
    if (maxTailBytes > 0 && m_submittedRenderTail.size() > maxTailBytes) {
        m_submittedRenderTail.remove(0, m_submittedRenderTail.size() - static_cast<int>(maxTailBytes));
    }
}

void WasapiOutputWorker::captureSeekResumeFirst50msSubmittedPcm(const QByteArray &submittedChunk,
                                                                 UINT32 writtenFrames,
                                                                 UINT32 paddingFrames,
                                                                 UINT32 availableFrames,
                                                                 bool warmup,
                                                                 bool silenceFill)
{
    const bool seekResumeContext =
        m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart");
    if (!seekResumeContext || m_seekResumeFirst50msLogged || submittedChunk.isEmpty()
        || writtenFrames == 0 || !m_deviceFormat.isValid() || m_deviceFormat.sampleRate <= 0) {
        return;
    }

    const int bytesPerFrame = m_deviceFormat.bytesPerFrame();
    if (bytesPerFrame <= 0) {
        return;
    }

    if (m_seekResumeFirst50msTargetFrames <= 0) {
        m_seekResumeFirst50msTargetFrames =
            qMax<qint64>(1, static_cast<qint64>(m_deviceFormat.sampleRate) * 50 / 1000);
    }

    const qint64 remainingFrames =
        m_seekResumeFirst50msTargetFrames - m_seekResumeFirst50msCapturedFrames;
    if (remainingFrames <= 0) {
        return;
    }

    const qint64 framesToCapture = qMin<qint64>(writtenFrames, remainingFrames);
    const qsizetype bytesToCapture =
        qMin<qsizetype>(submittedChunk.size(),
                        static_cast<qsizetype>(framesToCapture) * bytesPerFrame);
    if (bytesToCapture <= 0) {
        return;
    }

    m_seekResumeFirst50msSubmittedPcm.append(submittedChunk.constData(), bytesToCapture);
    m_seekResumeFirst50msCapturedFrames += bytesToCapture / bytesPerFrame;
    if (silenceFill && warmup) {
        m_seekResumeFirst50msWarmupFrames += bytesToCapture / bytesPerFrame;
    } else if (silenceFill) {
        m_seekResumeFirst50msStartupSilenceFrames += bytesToCapture / bytesPerFrame;
    } else {
        m_seekResumeFirst50msRealPcmFrames += bytesToCapture / bytesPerFrame;
    }

    if (m_seekResumeFirst50msCapturedFrames < m_seekResumeFirst50msTargetFrames) {
        return;
    }

    m_seekResumeFirst50msLogged = true;
    const RenderedBlockMetrics metrics =
        renderedBlockMetricsForChunk(m_seekResumeFirst50msSubmittedPcm, m_deviceFormat);
    const bool artifactDetected =
        metrics.valid && (metrics.jump >= 0.10 || metrics.firstSamplePeak >= 0.03);
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("renderMirrorFirst50msAfterSeek session=%1 frames=%2 targetFrames=%3 peak=%4 rms=%5 jump=%6 startSample=%7 endSample=%8 startupSilenceFrames=%9 warmupFrames=%10 realPcmFrames=%11 artifactDetected=%12 wasapiPaddingFrames=%13 wasapiAvailableFrames=%14 pipelineStartProfile=%15 artifactPath=%16 raw=%17 metadata=%18")
                          .arg(m_sessionId)
                          .arg(m_seekResumeFirst50msCapturedFrames)
                          .arg(m_seekResumeFirst50msTargetFrames)
                          .arg(metricText(metrics.peak))
                          .arg(metricText(metrics.rms))
                          .arg(metricText(metrics.jump))
                          .arg(fineMetricText(metrics.firstSample))
                          .arg(fineMetricText(metrics.lastSample))
                          .arg(m_seekResumeFirst50msStartupSilenceFrames)
                          .arg(m_seekResumeFirst50msWarmupFrames)
                          .arg(m_seekResumeFirst50msRealPcmFrames)
                          .arg(artifactDetected ? 1 : 0)
                          .arg(paddingFrames)
                          .arg(availableFrames)
                          .arg(m_artifactTracking.pipelineStartupProfile)
                          .arg(m_artifactTracking.artifactPath)
                          .arg(m_renderMirrorRawPath)
                          .arg(m_renderMirrorMetadataPath));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("seek_resume_render_mirror_first_50ms"),
                             {
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("frames"), m_seekResumeFirst50msCapturedFrames},
                                 {QStringLiteral("targetFrames"), m_seekResumeFirst50msTargetFrames},
                                 {QStringLiteral("peak"), metrics.peak},
                                 {QStringLiteral("rms"), metrics.rms},
                                 {QStringLiteral("jump"), metrics.jump},
                                 {QStringLiteral("startSample"), metrics.firstSample},
                                 {QStringLiteral("endSample"), metrics.lastSample},
                                 {QStringLiteral("startupSilenceFrames"), m_seekResumeFirst50msStartupSilenceFrames},
                                 {QStringLiteral("warmupFrames"), m_seekResumeFirst50msWarmupFrames},
                                 {QStringLiteral("realPcmFrames"), m_seekResumeFirst50msRealPcmFrames},
                                 {QStringLiteral("artifactDetected"), artifactDetected},
                                 {QStringLiteral("pipelineStartProfile"), m_artifactTracking.pipelineStartupProfile},
                                 {QStringLiteral("artifactPath"), m_artifactTracking.artifactPath},
                                 {QStringLiteral("rawPath"), m_renderMirrorRawPath},
                                 {QStringLiteral("metadataPath"), m_renderMirrorMetadataPath},
                                 {QStringLiteral("observationLayer"), QStringLiteral("WASAPI submitted PCM before endpoint output")},
                             });
}

void WasapiOutputWorker::mirrorSubmittedBlock(const QByteArray &submittedChunk,
                                               UINT32 writtenFrames,
                                               UINT32 paddingFrames,
                                               UINT32 availableFrames,
                                               const QString &renderSource,
                                               bool warmup,
                                               bool silenceFill,
                                               bool firstDataBlock)
{
    appendSubmittedPcmTail(submittedChunk, m_deviceFormat);
    captureSeekResumeFirst50msSubmittedPcm(submittedChunk,
                                           writtenFrames,
                                           paddingFrames,
                                           availableFrames,
                                           warmup,
                                           silenceFill);
    if (!m_renderMirrorActive || submittedChunk.isEmpty() || writtenFrames == 0) {
        return;
    }

    if (!m_artifactTracking.enabled) {
        return;
    }

    if (m_renderMirrorFile.isOpen()) {
        m_renderMirrorFile.write(submittedChunk);
    }

    const auto playbackContext = artifactPlaybackContext(paddingFrames);
    auto renderContext = artifactRenderContext(writtenFrames,
                                               paddingFrames,
                                               availableFrames,
                                               warmup,
                                               silenceFill,
                                               firstDataBlock);
    if (silenceFill) {
        m_renderMirrorMonitor.observeSilentFrames(writtenFrames,
                                                  m_deviceFormat,
                                                  playbackContext,
                                                  QStringLiteral("render-mirror:%1").arg(renderSource),
                                                  renderContext);
    } else {
        m_renderMirrorMonitor.analyzePcmBlock(submittedChunk.constData(),
                                              submittedChunk.size(),
                                              m_deviceFormat,
                                              playbackContext,
                                              QStringLiteral("render-mirror:%1").arg(renderSource),
                                              renderContext);
    }

    m_renderMirrorCapturedFrames += writtenFrames;
    if (m_renderMirrorCapturedFrames >= m_renderMirrorMaxFrames) {
        finishRenderMirrorCapture(QStringLiteral("window-complete"));
    }
}

void WasapiOutputWorker::observeArtifactSilence(UINT32 frameCount,
                                                 UINT32 paddingFrames,
                                                 UINT32 availableFrames,
                                                 bool warmup,
                                                 const QString &renderSource)
{
    if (!artifactTrackingEnabled()) {
        return;
    }

    m_artifactMonitor.observeSilentFrames(frameCount,
                                          m_bufferFormat,
                                          artifactPlaybackContext(paddingFrames),
                                          renderSource,
                                          artifactRenderContext(frameCount,
                                                                paddingFrames,
                                                                availableFrames,
                                                                warmup,
                                                                true,
                                                                false));
}

void WasapiOutputWorker::analyzeArtifactBlock(const QByteArray &chunk,
                                               UINT32 writtenFrames,
                                               UINT32 paddingFrames,
                                               UINT32 availableFrames,
                                               const QString &renderSource)
{
    if (chunk.isEmpty() || writtenFrames == 0) {
        return;
    }

    if (!artifactTrackingEnabled() && !PlayerLogger::highVolumeJsonlDiagnosticsEnabled()) {
        return;
    }

    const RenderedBlockMetrics metrics = renderedBlockMetricsForChunk(chunk);
    const bool firstDataBlock = m_firstDataBlockAfterConfigure;
    const bool activeSwitchContext =
        m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild");
    const bool seekContext =
        m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekResume")
        || m_artifactTracking.pipelineStartupProfile == QStringLiteral("SeekRestart");
    const bool suspiciousDiscontinuity =
        metrics.valid && (metrics.jump >= 0.10 || (firstDataBlock && metrics.firstSamplePeak >= 0.03));
    if (PlayerLogger::highVolumeJsonlDiagnosticsEnabled()
        && metrics.valid
        && (firstDataBlock || suspiciousDiscontinuity)) {
        PlayerLogger::diagnostic(QStringLiteral("audio"),
                                 QStringLiteral("internal_pcm_glitch_monitor"),
                                 {
                                     {QStringLiteral("sessionId"), m_sessionId},
                                     {QStringLiteral("positionMs"), artifactPlaybackContext(paddingFrames).positionMs},
                                     {QStringLiteral("transactionKind"), activeSwitchContext
                                          ? QStringLiteral("active-switch")
                                          : (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery")
                                                 ? QStringLiteral("recovery")
                                                 : (seekContext ? QStringLiteral("seek") : QStringLiteral("none")))},
                                     {QStringLiteral("pipelineStartProfile"), m_artifactTracking.pipelineStartupProfile},
                                     {QStringLiteral("activeSwitchTrigger"), m_artifactTracking.activeSwitchTrigger},
                                     {QStringLiteral("activeSwitchPhase"), m_artifactTracking.activeSwitchPhase},
                                     {QStringLiteral("activeSwitchReason"), m_artifactTracking.activeSwitchReason},
                                     {QStringLiteral("renderSource"), renderSource},
                                     {QStringLiteral("frames"), static_cast<qint64>(metrics.frameCount)},
                                     {QStringLiteral("peak"), metrics.peak},
                                     {QStringLiteral("rms"), metrics.rms},
                                     {QStringLiteral("maxSampleDelta"), metrics.jump},
                                     {QStringLiteral("firstSamplePeak"), metrics.firstSamplePeak},
                                     {QStringLiteral("lastSamplePeak"), metrics.lastSamplePeak},
                                     {QStringLiteral("duringActiveOutputSwitch"), activeSwitchContext},
                                     {QStringLiteral("suspiciousDiscontinuity"), suspiciousDiscontinuity},
                                     {QStringLiteral("observationLayer"), QStringLiteral("internal PCM before OS/audio backend output path")},
                                 });
    }
    if (artifactTrackingEnabled()) {
        if (firstDataBlock) {
            PlayerLogger::log(QStringLiteral("audio"),
                              QStringLiteral("firstDataBlockAfterConfigure session=%1 frames=%2 writeFrames=%3 wasapiPaddingFrames=%4 wasapiAvailableFrames=%5 peak=%6 rms=%7 jump=%8 pipelineStartProfile=%9 artifactPath=%10 activeSwitchTrigger=%11 activeSwitchPhase=%12 activeSwitchReason=%13 activeSwitchBoundaryPolicy=%14 firstBlockMaxFadeGain=%15")
                                  .arg(m_sessionId)
                                  .arg(writtenFrames)
                                  .arg(writtenFrames)
                                  .arg(paddingFrames)
                                  .arg(availableFrames)
                                  .arg(metricText(metrics.peak))
                                  .arg(metricText(metrics.rms))
                                  .arg(metricText(metrics.jump))
                                  .arg(m_artifactTracking.pipelineStartupProfile)
                                  .arg(m_artifactTracking.artifactPath)
                                  .arg(m_artifactTracking.activeSwitchTrigger)
                                  .arg(m_artifactTracking.activeSwitchPhase)
                                  .arg(m_artifactTracking.activeSwitchReason)
                                  .arg(m_activeSwitchBoundaryPolicyName)
                                  .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
            if (m_artifactTracking.pipelineStartupProfile == QStringLiteral("ActiveSwitchRebuild")) {
                const bool bridgeValid = m_activeSwitchEntryBridgeBlock.valid;
                const double previousToBridgeEnvelopeStep =
                    m_previousRenderedBlock.valid && bridgeValid
                    ? qMax(0.0,
                           m_previousRenderedBlock.lastSamplePeak
                               - m_activeSwitchEntryBridgeBlock.firstSamplePeak)
                    : 0.0;
                const double bridgeToFirstEnvelopeStep =
                    bridgeValid
                    ? std::abs(metrics.firstSamplePeak - m_activeSwitchEntryBridgeBlock.lastSamplePeak)
                    : 0.0;
                const double previousToFirstPeakDrop =
                    m_previousRenderedBlock.valid
                    ? qMax(0.0, m_previousRenderedBlock.peak - metrics.firstSamplePeak)
                    : 0.0;
                const bool popCandidate = m_previousRenderedBlock.valid
                    && (previousToBridgeEnvelopeStep >= 0.0200
                        || m_previousRenderedBlock.jump >= 0.0350
                        || m_previousRenderedBlock.lastSamplePeak >= 0.0300);
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("activeSwitchBoundaryEnvelope session=%1 previousSession=%2 previousValid=%3 previousFrames=%4 previousPeak=%5 previousRms=%6 previousJump=%7 previousFirstSamplePeak=%8 previousLastSamplePeak=%9 startupSilenceFrames=%10 startupSilenceMs=%11 firstFrames=%12 firstPeak=%13 firstRms=%14 firstJump=%15 firstFirstSamplePeak=%16 firstLastSamplePeak=%17 firstBlockEndFadeGain=%18 writeFrames=%19 wasapiPaddingFrames=%20 wasapiAvailableFrames=%21 pipelineStartProfile=%22 artifactPath=%23 activeSwitchTrigger=%24 activeSwitchPhase=%25 activeSwitchReason=%26 activeSwitchBoundaryPolicy=%27 firstBlockMaxFadeGain=%28")
                                      .arg(m_sessionId)
                                      .arg(m_previousRenderedBlock.sessionId)
                                      .arg(m_previousRenderedBlock.valid ? 1 : 0)
                                      .arg(m_previousRenderedBlock.frameCount)
                                      .arg(metricText(m_previousRenderedBlock.peak))
                                      .arg(metricText(m_previousRenderedBlock.rms))
                                      .arg(metricText(m_previousRenderedBlock.jump))
                                      .arg(metricText(m_previousRenderedBlock.firstSamplePeak))
                                      .arg(metricText(m_previousRenderedBlock.lastSamplePeak))
                                      .arg(m_configuredStartupSilenceFrames)
                                      .arg(m_configuredStartupSilenceMs)
                                      .arg(metrics.frameCount)
                                      .arg(metricText(metrics.peak))
                                      .arg(metricText(metrics.rms))
                                      .arg(metricText(metrics.jump))
                                      .arg(metricText(metrics.firstSamplePeak))
                                      .arg(metricText(metrics.lastSamplePeak))
                                      .arg(metricText(currentFadeEndpointGain()))
                                      .arg(writtenFrames)
                                      .arg(paddingFrames)
                                      .arg(availableFrames)
                                      .arg(m_artifactTracking.pipelineStartupProfile)
                                      .arg(m_artifactTracking.artifactPath)
                                      .arg(m_artifactTracking.activeSwitchTrigger)
                                      .arg(m_artifactTracking.activeSwitchPhase)
                                      .arg(m_artifactTracking.activeSwitchReason)
                                      .arg(m_activeSwitchBoundaryPolicyName)
                                      .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
                PlayerLogger::log(QStringLiteral("audio"),
                                  QStringLiteral("activeSwitchBoundaryPopCandidate session=%1 candidate=%2 previousValid=%3 bridgeValid=%4 fallbackToSilence=%5 previousToBridgeEnvelopeStep=%6 bridgeToFirstEnvelopeStep=%7 previousToFirstPeakDrop=%8 previousPeak=%9 previousJump=%10 previousLastSamplePeak=%11 bridgePeak=%12 bridgeJump=%13 bridgeFirstSamplePeak=%14 bridgeLastSamplePeak=%15 firstPeak=%16 firstJump=%17 firstFirstSamplePeak=%18 firstBlockEndFadeGain=%19 startupSilenceFrames=%20 pipelineStartProfile=%21 artifactPath=%22 activeSwitchTrigger=%23 activeSwitchReason=%24 activeSwitchBoundaryPolicy=%25 firstBlockMaxFadeGain=%26")
                                      .arg(m_sessionId)
                                      .arg(popCandidate ? 1 : 0)
                                      .arg(m_previousRenderedBlock.valid ? 1 : 0)
                                      .arg(bridgeValid ? 1 : 0)
                                      .arg(m_activeSwitchEntryBridgeFallback ? 1 : 0)
                                      .arg(metricText(previousToBridgeEnvelopeStep))
                                      .arg(metricText(bridgeToFirstEnvelopeStep))
                                      .arg(metricText(previousToFirstPeakDrop))
                                      .arg(metricText(m_previousRenderedBlock.peak))
                                      .arg(metricText(m_previousRenderedBlock.jump))
                                      .arg(metricText(m_previousRenderedBlock.lastSamplePeak))
                                      .arg(metricText(m_activeSwitchEntryBridgeBlock.peak))
                                      .arg(metricText(m_activeSwitchEntryBridgeBlock.jump))
                                      .arg(metricText(m_activeSwitchEntryBridgeBlock.firstSamplePeak))
                                      .arg(metricText(m_activeSwitchEntryBridgeBlock.lastSamplePeak))
                                      .arg(metricText(metrics.peak))
                                      .arg(metricText(metrics.jump))
                                      .arg(metricText(metrics.firstSamplePeak))
                                      .arg(metricText(currentFadeEndpointGain()))
                                      .arg(m_configuredStartupSilenceFrames)
                                      .arg(m_artifactTracking.pipelineStartupProfile)
                                      .arg(m_artifactTracking.artifactPath)
                                      .arg(m_artifactTracking.activeSwitchTrigger)
                                      .arg(m_artifactTracking.activeSwitchReason)
                                      .arg(m_activeSwitchBoundaryPolicyName)
                                      .arg(QString::number(m_activeSwitchFirstBlockMaxFadeGain, 'f', 2)));
            }
        }
        m_artifactMonitor.analyzePcmBlock(chunk.constData(),
                                          chunk.size(),
                                          m_bufferFormat,
                                          artifactPlaybackContext(paddingFrames),
                                          renderSource,
                                          artifactRenderContext(writtenFrames,
                                                                paddingFrames,
                                                                availableFrames,
                                                                false,
                                                                false,
                                                                firstDataBlock));
    }
    m_firstDataBlockAfterConfigure = false;
    m_lastRenderedBlock = metrics;
}

AudioArtifactMonitor::PlaybackContext WasapiOutputWorker::artifactPlaybackContext(UINT32 paddingFrames) const
{
    AudioArtifactMonitor::PlaybackContext context;
    context.sessionId = m_sessionId;
    context.source = m_artifactTracking.source;
    context.playbackState = m_artifactTracking.playbackState;
    context.recentControlEvent = m_artifactTracking.recentControlEvent;
    context.positionMs = m_artifactTracking.startPositionMs + processedPositionMsFromPadding(paddingFrames);
    context.recoveryPending = m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery");
    context.recoveryAttempt = m_artifactTracking.recoveryAttempt;
    context.pipelineStartupProfile = m_artifactTracking.pipelineStartupProfile;
    context.artifactPath = m_artifactTracking.artifactPath;
    context.activeSwitchTrigger = m_artifactTracking.activeSwitchTrigger;
    context.activeSwitchPhase = m_artifactTracking.activeSwitchPhase;
    context.activeSwitchReason = m_artifactTracking.activeSwitchReason;
    return context;
}

AudioArtifactMonitor::RenderContext WasapiOutputWorker::artifactRenderContext(UINT32 writeFrameCount,
                                                          UINT32 paddingFrames,
                                                          UINT32 availableFrames,
                                                          bool warmup,
                                                          bool silenceFill,
                                                          bool firstDataBlock) const
{
    AudioArtifactMonitor::RenderContext context;
    context.warmup = warmup;
    context.silenceFill = silenceFill;
    context.recovery = m_artifactTracking.pipelineStartupProfile == QStringLiteral("ErrorRecovery");
    context.firstDataBlockAfterConfigure = firstDataBlock;
    context.writeFrameCount = writeFrameCount;
    context.wasapiPaddingFrames = paddingFrames;
    context.wasapiAvailableFrames = availableFrames;
    return context;
}

void WasapiOutputWorker::noteFirstSubmittedPcmAfterSeek(UINT32 writtenFrames,
                                                         UINT32 paddingFrames,
                                                         UINT32 availableFrames,
                                                         const RenderedBlockMetrics &metrics,
                                                         const PcmFadeApplication &fade)
{
    if (m_artifactTracking.pipelineStartupProfile != QStringLiteral("SeekResume")
        || m_seekResumeFirstSubmittedPcmAfterSeekMs >= 0) {
        return;
    }

    m_seekResumeFirstSubmittedPcmAfterSeekMs = QDateTime::currentMSecsSinceEpoch();
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("seekResumeTiming marker=first-submitted session=%1 seekRequestTimeMs=%2 pipelineStartTimeMs=%3 firstDecodedPcmAfterSeekMs=%4 firstSubmittedPcmAfterSeekMs=%5 writeFrames=%6 wasapiPaddingFrames=%7 wasapiAvailableFrames=%8 seekResumeStartupSilenceMs=%9 seekResumeWarmupDiscardMs=%10 seekResumeFadeInMs=%11 realtimeDecodeEnabled=%12")
                          .arg(m_sessionId)
                          .arg(m_artifactTracking.seekRequestTimeMs)
                          .arg(m_artifactTracking.pipelineStartTimeMs)
                          .arg(m_seekResumeFirstDecodedPcmAfterSeekMs)
                          .arg(m_seekResumeFirstSubmittedPcmAfterSeekMs)
                          .arg(writtenFrames)
                          .arg(paddingFrames)
                          .arg(availableFrames)
                          .arg(m_configuredStartupSilenceMs)
                          .arg(m_configuredWarmupDiscardMs)
                          .arg(m_pcmFadeInDurationMs)
                          .arg(m_artifactTracking.realtimeDecodeEnabled ? 1 : 0));
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("seekResumeFirstSubmittedBlock session=%1 firstSubmittedBlockPeak=%2 firstSubmittedBlockStartSample=%3 firstSubmittedBlockEndSample=%4 firstSubmittedBlockJump=%5 firstSubmittedBlockFadeApplied=%6 firstSubmittedBlockMinGain=%7 firstSubmittedBlockMaxGain=%8 firstSubmittedBlockFadeFrames=%9 firstSubmittedBlockFadeTotalFrames=%10 firstSubmittedBlockFadeFramesBefore=%11 firstSubmittedBlockFadeFramesAfter=%12 writeFrames=%13 wasapiPaddingFrames=%14 wasapiAvailableFrames=%15 seekResumeStartupSilenceMs=%16 seekResumeWarmupDiscardMs=%17 seekResumeFadeInMs=%18 pipelineStartProfile=%19 artifactPath=%20")
                          .arg(m_sessionId)
                          .arg(metricText(metrics.peak))
                          .arg(fineMetricText(metrics.firstSample))
                          .arg(fineMetricText(metrics.lastSample))
                          .arg(metricText(metrics.jump))
                          .arg(fade.applied ? 1 : 0)
                          .arg(fineMetricText(fade.minGain))
                          .arg(fineMetricText(fade.maxGain))
                          .arg(fade.frames)
                          .arg(fade.totalFrames)
                          .arg(fade.framesProcessedBefore)
                          .arg(fade.framesProcessedAfter)
                          .arg(writtenFrames)
                          .arg(paddingFrames)
                          .arg(availableFrames)
                          .arg(m_configuredStartupSilenceMs)
                          .arg(m_configuredWarmupDiscardMs)
                          .arg(m_pcmFadeInDurationMs)
                          .arg(m_artifactTracking.pipelineStartupProfile)
                          .arg(m_artifactTracking.artifactPath));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("seek_resume_first_submitted_pcm"),
                             {
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("seekRequestTimeMs"), m_artifactTracking.seekRequestTimeMs},
                                 {QStringLiteral("pipelineStartTimeMs"), m_artifactTracking.pipelineStartTimeMs},
                                 {QStringLiteral("firstDecodedPcmAfterSeekMs"), m_seekResumeFirstDecodedPcmAfterSeekMs},
                                 {QStringLiteral("firstSubmittedPcmAfterSeekMs"), m_seekResumeFirstSubmittedPcmAfterSeekMs},
                                 {QStringLiteral("writeFrames"), static_cast<qint64>(writtenFrames)},
                                 {QStringLiteral("wasapiPaddingFrames"), static_cast<qint64>(paddingFrames)},
                                 {QStringLiteral("wasapiAvailableFrames"), static_cast<qint64>(availableFrames)},
                                 {QStringLiteral("seekResumeStartupSilenceMs"), static_cast<int>(m_configuredStartupSilenceMs)},
                                 {QStringLiteral("seekResumeWarmupDiscardMs"), static_cast<int>(m_configuredWarmupDiscardMs)},
                                 {QStringLiteral("seekResumeFadeInMs"), m_pcmFadeInDurationMs},
                                 {QStringLiteral("firstSubmittedBlockPeak"), metrics.peak},
                                 {QStringLiteral("firstSubmittedBlockStartSample"), metrics.firstSample},
                                 {QStringLiteral("firstSubmittedBlockEndSample"), metrics.lastSample},
                                 {QStringLiteral("firstSubmittedBlockJump"), metrics.jump},
                                 {QStringLiteral("firstSubmittedBlockFadeApplied"), fade.applied},
                                 {QStringLiteral("firstSubmittedBlockMinGain"), fade.minGain},
                                 {QStringLiteral("firstSubmittedBlockMaxGain"), fade.maxGain},
                                 {QStringLiteral("firstSubmittedBlockFadeFrames"), static_cast<qint64>(fade.frames)},
                                 {QStringLiteral("firstSubmittedBlockFadeTotalFrames"), static_cast<qint64>(fade.totalFrames)},
                                 {QStringLiteral("realtimeDecodeEnabled"), m_artifactTracking.realtimeDecodeEnabled},
                             });
}

void WasapiOutputWorker::logSeekResumeLatencyIfNeeded(qint64 firstAudibleOrFadeOpenMs)
{
    if (m_artifactTracking.pipelineStartupProfile != QStringLiteral("SeekResume")
        || m_seekResumeLatencyLogged) {
        return;
    }

    m_seekResumeLatencyLogged = true;
    const qint64 seekResumeLatencyMs =
        m_artifactTracking.seekRequestTimeMs >= 0 && firstAudibleOrFadeOpenMs >= 0
        ? firstAudibleOrFadeOpenMs - m_artifactTracking.seekRequestTimeMs
        : -1;
    PlayerLogger::log(QStringLiteral("audio"),
                      QStringLiteral("seekResumeLatency session=%1 seekRequestTimeMs=%2 pipelineStartTimeMs=%3 firstDecodedPcmAfterSeekMs=%4 firstSubmittedPcmAfterSeekMs=%5 firstAudibleOrFadeOpenMs=%6 seekResumeLatencyMs=%7 seekResumeStartupSilenceMs=%8 seekResumeWarmupDiscardMs=%9 seekResumeFadeInMs=%10 realtimeDecodeEnabled=%11 streamFadeInDelayMs=%12")
                          .arg(m_sessionId)
                          .arg(m_artifactTracking.seekRequestTimeMs)
                          .arg(m_artifactTracking.pipelineStartTimeMs)
                          .arg(m_seekResumeFirstDecodedPcmAfterSeekMs)
                          .arg(m_seekResumeFirstSubmittedPcmAfterSeekMs)
                          .arg(firstAudibleOrFadeOpenMs)
                          .arg(seekResumeLatencyMs)
                          .arg(m_configuredStartupSilenceMs)
                          .arg(m_configuredWarmupDiscardMs)
                          .arg(m_pcmFadeInDurationMs)
                          .arg(m_artifactTracking.realtimeDecodeEnabled ? 1 : 0)
                          .arg(m_streamFadeInDelayMs));
    PlayerLogger::diagnostic(QStringLiteral("audio"),
                             QStringLiteral("seek_resume_latency"),
                             {
                                 {QStringLiteral("sessionId"), m_sessionId},
                                 {QStringLiteral("seekRequestTimeMs"), m_artifactTracking.seekRequestTimeMs},
                                 {QStringLiteral("pipelineStartTimeMs"), m_artifactTracking.pipelineStartTimeMs},
                                 {QStringLiteral("firstDecodedPcmAfterSeekMs"), m_seekResumeFirstDecodedPcmAfterSeekMs},
                                 {QStringLiteral("firstSubmittedPcmAfterSeekMs"), m_seekResumeFirstSubmittedPcmAfterSeekMs},
                                 {QStringLiteral("firstAudibleOrFadeOpenMs"), firstAudibleOrFadeOpenMs},
                                 {QStringLiteral("seekResumeLatencyMs"), seekResumeLatencyMs},
                                 {QStringLiteral("seekResumeStartupSilenceMs"), static_cast<int>(m_configuredStartupSilenceMs)},
                                 {QStringLiteral("seekResumeWarmupDiscardMs"), static_cast<int>(m_configuredWarmupDiscardMs)},
                                 {QStringLiteral("seekResumeFadeInMs"), m_pcmFadeInDurationMs},
                                 {QStringLiteral("realtimeDecodeEnabled"), m_artifactTracking.realtimeDecodeEnabled},
                                 {QStringLiteral("streamFadeInDelayMs"), m_streamFadeInDelayMs},
                             });
}
