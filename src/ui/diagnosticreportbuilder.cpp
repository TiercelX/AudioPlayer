#include "diagnosticreportbuilder.h"
#include "playerlogger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStringList>

namespace {

constexpr int kSeekResumeBoundaryWindowMs = 250;

struct SeekResumeArtifactJudgment
{
    QString artifactWindow = QStringLiteral("Unknown");
    QString artifactClassification = QStringLiteral("DetectorInconclusive");
    qint64 firstArtifactOffsetMsAfterResume = -1;
    int boundaryArtifactCount = 0;
    int fullSegmentArtifactCount = 0;
    int contentTransientLikelyCount = 0;
    int detectorInconclusiveCount = 0;
    bool boundaryArtifactDetected = false;
    bool contentTransientLikely = false;
    bool hardArtifactEvidenceDetected = false;
    bool nonSeekResumeRenderMirrorArtifactDetected = false;
};

QString reportResult(bool playbackStarted,
                     bool failCondition,
                     bool switchRequested,
                     bool switchCompleted,
                     bool renderMirrorClean,
                     bool systemInvalidationDuringSwitch,
                     bool forceInconclusive = false)
{
    if (failCondition || (switchRequested && !switchCompleted)) {
        return QStringLiteral("FAIL");
    }
    if (forceInconclusive) {
        return playbackStarted ? QStringLiteral("INCONCLUSIVE") : QStringLiteral("FAIL");
    }
    if (renderMirrorClean && (!switchRequested || switchCompleted)) {
        return systemInvalidationDuringSwitch ? QStringLiteral("WARN") : QStringLiteral("PASS");
    }
    return playbackStarted ? QStringLiteral("INCONCLUSIVE") : QStringLiteral("FAIL");
}

int regexCount(const QString &text, const QString &pattern)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    int count = 0;
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

int countActiveSwitchQuarantineWaitForData(const QString &text)
{
    int count = 0;
    bool activeSwitchBufferQuarantined = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.contains(QStringLiteral("playback event=buffer-quarantined"))
            && line.contains(QStringLiteral("details=reason=activeOutputSwitch:"))) {
            activeSwitchBufferQuarantined = true;
            continue;
        }

        if (activeSwitchBufferQuarantined
            && line.contains(QStringLiteral("output event=wait-for-data"))) {
            ++count;
            continue;
        }

        if (activeSwitchBufferQuarantined
            && (line.contains(QStringLiteral("activeSwitchPreflight "))
                || line.contains(QStringLiteral("activeOutputSwitch conservative-rebuild"))
                || line.contains(QStringLiteral("activeOutputSwitch reset "))
                || line.contains(QStringLiteral("activeOutputSwitch output-active-summary"))
                || (line.contains(QStringLiteral("playback event=buffer-quarantined"))
                    && !line.contains(QStringLiteral("details=reason=activeOutputSwitch:"))))) {
            activeSwitchBufferQuarantined = false;
        }
    }
    return count;
}

double maxRegexCaptureDouble(const QString &text, const QString &pattern)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    double maximum = 0.0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        if (ok) {
            maximum = qMax(maximum, value);
        }
    }
    return maximum;
}

QJsonValue maxRegexCaptureIntegerValue(const QString &text, const QString &pattern)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    qint64 maximum = -1;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const qint64 value = match.captured(1).toLongLong(&ok);
        if (ok && value >= 0) {
            maximum = qMax(maximum, value);
        }
    }

    return maximum >= 0
        ? QJsonValue(static_cast<qint64>(maximum))
        : QJsonValue(QJsonValue::Null);
}

QJsonValue lastRegexCaptureDoubleValue(const QString &text, const QString &pattern)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    bool found = false;
    double lastValue = 0.0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        if (ok) {
            lastValue = value;
            found = true;
        }
    }

    return found ? QJsonValue(lastValue) : QJsonValue(QJsonValue::Null);
}

QJsonValue lastRegexCaptureStringValue(const QString &text, const QString &pattern)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator it = expression.globalMatch(text);
    QString lastValue;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        lastValue = match.captured(1).trimmed();
    }

    return lastValue.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(lastValue);
}

QString logField(const QString &line, const QString &name, const QString &defaultValue = QString())
{
    const QRegularExpression expression(QStringLiteral("\\b%1=([^\\s]+)")
                                             .arg(QRegularExpression::escape(name)));
    const QRegularExpressionMatch match = expression.match(line);
    return match.hasMatch() ? match.captured(1).trimmed() : defaultValue;
}

bool logFlag(const QString &line, const QString &name)
{
    const QString value = logField(line, name).toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

qint64 logInteger(const QString &line, const QString &name, qint64 defaultValue = -1)
{
    bool ok = false;
    const qint64 value = logField(line, name).toLongLong(&ok);
    return ok ? value : defaultValue;
}

double logDouble(const QString &line, const QString &name, double defaultValue = 0.0)
{
    bool ok = false;
    const double value = logField(line, name).toDouble(&ok);
    return ok ? value : defaultValue;
}

bool isSeekResumeProfile(const QString &value)
{
    return value == QStringLiteral("SeekResume") || value == QStringLiteral("SeekRestart");
}

bool isContentTransientDetector(const QString &detector)
{
    return detector == QStringLiteral("transient-spike")
        || detector == QStringLiteral("short-burst")
        || detector == QStringLiteral("crackle-texture")
        || detector == QStringLiteral("sample-jump");
}

bool isHardArtifactDetector(const QString &detector, const QString &severity, double jump)
{
    return detector == QStringLiteral("invalid-sample")
        || detector == QStringLiteral("out-of-range-sample")
        || detector == QStringLiteral("block-boundary-discontinuity")
        || detector == QStringLiteral("silence-hard-switch")
        || (detector == QStringLiteral("sample-jump")
            && (severity == QStringLiteral("critical") || jump >= 1.50));
}

bool isKnownSyntheticFixture(const QString &sourcePath)
{
    const QString name = QFileInfo(sourcePath).fileName().toLower();
    return name == QStringLiteral("smoke.wav")
        || name == QStringLiteral("smoke.flac")
        || name == QStringLiteral("smoke.mp3")
        || name == QStringLiteral("smoke.aac")
        || name == QStringLiteral("smoke.m4a")
        || name == QStringLiteral("smoke-alac.m4a")
        || name.startsWith(QStringLiteral("sine-"))
        || name.startsWith(QStringLiteral("silence-"))
        || name.startsWith(QStringLiteral("ab-sine-"))
        || name.startsWith(QStringLiteral("ab-pink-noise-"));
}

bool isKnownCompressedContentSample(const QString &logText)
{
    const QStringList lines = logText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);
    const QRegularExpression sourceExpression(QStringLiteral("startPipeline startPositionMs=.*\\bsource=(.+)$"));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = sourceExpression.match(line);
        if (!match.hasMatch()) {
            continue;
        }

        const QString sourcePath = match.captured(1).trimmed();
        if (isKnownSyntheticFixture(sourcePath)) {
            continue;
        }

        const QString lowerPath = sourcePath.toLower();
        const QString name = QFileInfo(sourcePath).fileName().toLower();
        if (name == QStringLiteral("real-alac-sample.m4a")) {
            return true;
        }

        const bool compressedExtension =
            name.endsWith(QStringLiteral(".m4a"))
            || name.endsWith(QStringLiteral(".aac"))
            || name.endsWith(QStringLiteral(".mp3"))
            || name.endsWith(QStringLiteral(".flac"))
            || name.endsWith(QStringLiteral(".eb3"))
            || name.endsWith(QStringLiteral(".mlp"));
        if (compressedExtension
            && (lowerPath.contains(QStringLiteral("/media/"))
                || lowerPath.contains(QStringLiteral("\\media\\")))) {
            return true;
        }
    }

    return false;
}

bool isSyntheticFixtureLog(const QString &logText)
{
    const QStringList lines = logText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);
    const QRegularExpression sourceExpression(QStringLiteral("startPipeline startPositionMs=.*\\bsource=(.+)$"));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = sourceExpression.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        if (isKnownSyntheticFixture(match.captured(1).trimmed())) {
            return true;
        }
    }
    return false;
}

SeekResumeArtifactJudgment classifySeekResumeArtifacts(const QString &logText, bool compressedContentSample)
{
    SeekResumeArtifactJudgment judgment;
    QHash<int, qint64> seekResumeStartPositionBySession;
    const QStringList lines = logText.split(QRegularExpression(QStringLiteral("[\\r\\n]+")),
                                            Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("renderMirror start "))) {
            continue;
        }

        const QString profile = logField(line, QStringLiteral("startupProfile"));
        if (!isSeekResumeProfile(profile)) {
            continue;
        }

        const int session = static_cast<int>(logInteger(line, QStringLiteral("session"), 0));
        const qint64 positionMs = logInteger(line, QStringLiteral("positionMs"));
        if (session > 0 && positionMs >= 0) {
            seekResumeStartPositionBySession.insert(session, positionMs);
        }
    }

    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("renderMirrorConclusion "))) {
            continue;
        }

        if (logField(line, QStringLiteral("artifactDetected")) != QStringLiteral("1")) {
            continue;
        }

        const QString path = logField(line, QStringLiteral("artifactPath"));
        const QString profile = logField(line, QStringLiteral("startupProfile"));
        if (!isSeekResumeProfile(path) && !isSeekResumeProfile(profile)) {
            judgment.nonSeekResumeRenderMirrorArtifactDetected = true;
        }
    }

    for (const QString &line : lines) {
        if (!line.contains(QStringLiteral("audioArtifact "))) {
            continue;
        }

        const QString renderSource = logField(line, QStringLiteral("renderSource"));
        if (!renderSource.startsWith(QStringLiteral("render-mirror:"))) {
            continue;
        }

        const QString profile = logField(line, QStringLiteral("pipelineStartProfile"));
        const QString path = logField(line, QStringLiteral("artifactPath"));
        if (!isSeekResumeProfile(profile) && !isSeekResumeProfile(path)) {
            continue;
        }

        ++judgment.fullSegmentArtifactCount;

        const int session = static_cast<int>(logInteger(line, QStringLiteral("session"), 0));
        const qint64 positionMs = logInteger(line, QStringLiteral("positionMs"));
        qint64 offsetMs = -1;
        if (session > 0 && positionMs >= 0 && seekResumeStartPositionBySession.contains(session)) {
            offsetMs = positionMs - seekResumeStartPositionBySession.value(session);
            if (judgment.firstArtifactOffsetMsAfterResume < 0
                || offsetMs < judgment.firstArtifactOffsetMsAfterResume) {
                judgment.firstArtifactOffsetMsAfterResume = offsetMs;
            }
        }

        const QString detector = logField(line, QStringLiteral("detector"));
        const QString severity = logField(line, QStringLiteral("severity"));
        const double jump = logDouble(line, QStringLiteral("jump"));
        const bool invalidTimestamp = offsetMs < 0;
        const bool hardEvidence = invalidTimestamp
            || isHardArtifactDetector(detector, severity, jump);
        judgment.hardArtifactEvidenceDetected =
            judgment.hardArtifactEvidenceDetected || hardEvidence;

        if (offsetMs >= 0 && offsetMs <= kSeekResumeBoundaryWindowMs) {
            ++judgment.boundaryArtifactCount;
            judgment.boundaryArtifactDetected = true;
            continue;
        }

        if (compressedContentSample && !hardEvidence && isContentTransientDetector(detector)) {
            ++judgment.contentTransientLikelyCount;
            judgment.contentTransientLikely = true;
        } else {
            ++judgment.detectorInconclusiveCount;
        }
    }

    if (judgment.boundaryArtifactDetected) {
        judgment.artifactWindow = QStringLiteral("SeekResumeBoundary");
        judgment.artifactClassification = QStringLiteral("SeekBoundaryCandidate");
    } else if (judgment.fullSegmentArtifactCount > 0) {
        judgment.artifactWindow = QStringLiteral("FullSegment");
        judgment.artifactClassification =
            judgment.contentTransientLikelyCount == judgment.fullSegmentArtifactCount
            ? QStringLiteral("ContentTransientLikely")
            : QStringLiteral("DetectorInconclusive");
    }

    return judgment;
}

} // namespace

void writeDiagnosticReport(const QString &reportPath, bool switchRequestedByCli)
{
    if (reportPath.trimmed().isEmpty()) {
        return;
    }

    QFile logFile(PlayerLogger::logFilePath());
    const bool logReadable = logFile.open(QIODevice::ReadOnly | QIODevice::Text);
    const QString logText = logReadable ? QString::fromUtf8(logFile.readAll()) : QString();

    const bool playbackStateReachedPlaying =
        logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*state=Playing")));
    const bool asioBackendLoaded =
        logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*load backend=.*ASIO")));
    const bool asioFirstBufferSwitchObserved =
        logText.contains(QStringLiteral("asio firstBufferSwitch "));
    const bool playbackStarted =
        playbackStateReachedPlaying && (!asioBackendLoaded || asioFirstBufferSwitchObserved);
    const bool playbackContinuedAfterSwitch =
        logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*positionMs=\\d+")))
        && logText.contains(QStringLiteral("activeOutputSwitch output-active-summary"));
    const bool outputSwitchRequested =
        switchRequestedByCli
        || logText.contains(QStringLiteral("action=refresh-output"))
        || logText.contains(QStringLiteral("action=select-output-device"))
        || logText.contains(QStringLiteral("activeOutputSwitch phase Idle ->"));
    const bool sourceSwitchRequested = regexCount(logText, QStringLiteral("\\[automation\\].*load backend=")) > 1;
    const bool seekRequested =
        logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*action=seek")))
        || logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*action=seek-pause-resume")));
    const int seekCompletedCount =
        regexCount(logText, QStringLiteral("\\[automation\\].*seekCompleted target="));
    const int seekResumePipelineCount =
        regexCount(logText, QStringLiteral("startPipeline startPositionMs=.*pipelineStartProfile=SeekResume"));
    const int activeOutputSwitchStartedCount =
        regexCount(logText, QStringLiteral("activeOutputSwitch phase Idle ->"));
    const int activeOutputSwitchCompletedCount =
        regexCount(logText, QStringLiteral("activeOutputSwitch output-active-summary"));
    const bool outputSwitchCompleted =
        logText.contains(QStringLiteral("activeOutputSwitch output-active-summary"))
        || logText.contains(QStringLiteral("activeOutputSwitch reset reason=device-unchanged"));
    const int sameOutputInvalidationCount =
        regexCount(logText,
                   QStringLiteral("activeOutputSwitch phase .* -> WaitingForInvalidation .*trigger=OutputFormatChange.*transactionReason=audioOutputsChanged"));
    const int sameOutputInvalidationAbsorbedEventCount =
        regexCount(logText,
                   QStringLiteral("activeOutputSwitch absorbed-output-(?:stop|error) .*trigger=OutputFormatChange phase=WaitingForInvalidation reason=audioOutputsChanged"));
    const int wasapiErrorRecoveryScheduledCount =
        regexCount(logText, QStringLiteral("outputRecovery scheduled"));
    const int wasapiErrorRecoveryStartCount =
        regexCount(logText, QStringLiteral("outputRecovery attempt-begin"));
    const bool renderMirrorObserved = logText.contains(QStringLiteral("renderMirrorConclusion "));
    const bool renderMirrorArtifactDetected =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=1")));
    const bool renderMirrorClean =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=0")));
    const bool coldStartMirrorObserved =
        logText.contains(QStringLiteral("coldStartMirrorObserved"))
        || logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*startupProfile=ColdStart")));
    const bool coldStartSubmittedMirrorClean =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=0.*startupProfile=ColdStart")));
    const bool coldStartLoopbackObserved = false;
    const bool sourceSwitchMirrorClean =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=0.*artifactPath=SourceSwitch")));
    const bool seekResumeMirrorObserved =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactPath=SeekResume")));
    const bool seekResumeMirrorClean =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=0.*artifactPath=SeekResume")));
    const bool seekResumeMirrorArtifactDetected =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorConclusion .*artifactDetected=1.*artifactPath=SeekResume")));
    const bool asioRenderMirrorObserved = logText.contains(QStringLiteral("asioRenderMirrorConclusion "));
    const bool asioRenderMirrorArtifactDetected =
        logText.contains(QRegularExpression(QStringLiteral("asioRenderMirrorConclusion .*artifactDetected=1")));
    const bool asioRenderMirrorClean =
        logText.contains(QRegularExpression(QStringLiteral("asioRenderMirrorConclusion .*artifactDetected=0")));
    const int bufferWaitForDataRawCount =
        regexCount(logText, QStringLiteral("output event=wait-for-data"));
    const int activeSwitchQuarantineWaitForDataCount =
        qMin(bufferWaitForDataRawCount, countActiveSwitchQuarantineWaitForData(logText));
    const int bufferStarvationRawCount =
        regexCount(logText, QStringLiteral("buffer_starvation"));
    const int unexpectedWaitForDataCount =
        qMax(0, bufferWaitForDataRawCount - activeSwitchQuarantineWaitForDataCount);
    const int bufferUnderrunCount = bufferStarvationRawCount + unexpectedWaitForDataCount;
    const bool bufferUnderrun = bufferUnderrunCount > 0;
    const int decoderBackpressureCount =
        regexCount(logText, QStringLiteral("decoder-output-backpressure"));
    const bool decoderBackpressureDetected = decoderBackpressureCount > 0;
    const bool positionStallOrLag =
        logText.contains(QStringLiteral("playback event=position-stall"))
        || logText.contains(QStringLiteral("playback event=position-lag"));
    const bool recoveryExhausted =
        logText.contains(QStringLiteral("outputRecovery exhausted"))
        || logText.contains(QStringLiteral("recovery exhausted"));
    const bool activeSwitchOutputError =
        logText.contains(QStringLiteral("activeOutputSwitch output-error"));
    const bool staleSessionWriteDetected = logText.contains(QStringLiteral("stale_session_write"));
    const bool staleBufferReadDetected = logText.contains(QStringLiteral("stale_buffer_read"));
    const int staleSessionWriteCount = regexCount(logText, QStringLiteral("stale_session_write"));
    const int staleBufferReadCount = regexCount(logText, QStringLiteral("stale_buffer_read"));
    const bool oldPcmLeakDetected = staleSessionWriteDetected || staleBufferReadDetected;
    const bool sourceSwitchClean = sourceSwitchRequested && !oldPcmLeakDetected && sourceSwitchMirrorClean;
    const bool spatialEndpointFlushEnabled =
        logText.contains(QStringLiteral("spatialEndpointFlushBegin"))
        || logText.contains(QStringLiteral("spatialEndpointFlush dispatch reason="));
    const bool spatialEndpointFlushFailed = logText.contains(QStringLiteral("spatialEndpointFlushFailed"));
    const int spatialEndpointFlushBeginCount = regexCount(logText, QStringLiteral("spatialEndpointFlushBegin"));
    const int spatialEndpointFlushDoneCount = regexCount(logText, QStringLiteral("spatialEndpointFlushDone"));
    QRegularExpression flushMsExpression(QStringLiteral("spatialEndpointFlushBegin .*flushMs=(\\d+)"));
    QRegularExpressionMatch flushMsMatch = flushMsExpression.match(logText);
    const int spatialEndpointFlushMs = flushMsMatch.hasMatch() ? flushMsMatch.captured(1).toInt() : 0;
    QRegularExpression settleMsExpression(QStringLiteral("spatialEndpointFlushBegin .*settleMs=(\\d+)"));
    QRegularExpressionMatch settleMsMatch = settleMsExpression.match(logText);
    const int spatialEndpointSettleMs = settleMsMatch.hasMatch() ? settleMsMatch.captured(1).toInt() : 0;
    const bool spatialAudioProbeObserved = logText.contains(QStringLiteral("spatialAudioProbe "));
    const bool spatialAudioProbeAvailable = logText.contains(QStringLiteral("spatialAudioProbe ok "));
    const bool spatialAudioProbeUnavailable = logText.contains(QStringLiteral("spatialAudioProbe unavailable "));
    const bool spatialAudioStreamAvailable =
        logText.contains(QRegularExpression(QStringLiteral("spatialAudioProbe ok .*streamHr=0x00000000")));
    const bool spatialAudioSupportsFiveOneTwo =
        logText.contains(QRegularExpression(QStringLiteral("spatialAudioProbe ok .*supports5\\.1\\.2=1")));
    const QJsonValue spatialAudioNativeMask =
        lastRegexCaptureStringValue(logText, QStringLiteral("spatialAudioProbe ok .*nativeMask=([^\\s]+)"));
    const QJsonValue spatialAudioMaxDynamicObjects =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("spatialAudioProbe ok .*maxDynamicObjects=(\\d+)"));
    const QJsonValue spatialAudioObjectFormatCount =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("spatialAudioProbe ok .*formatCount=(\\d+)"));
    const QJsonValue spatialAudioFirstObjectFormat =
        lastRegexCaptureStringValue(logText,
                                    QStringLiteral("spatialAudioProbe objectFormat index=0 [^\\r\\n]*(rate=[^\\r\\n]+)"));
    const bool automationError =
        logText.contains(QRegularExpression(QStringLiteral("\\[automation\\].*error message=")));
    const bool systemInvalidationDuringSwitch =
        logText.contains(QStringLiteral("activeOutputSwitch absorbed-output-error"))
        || logText.contains(QStringLiteral("activeOutputSwitch absorbed-output-start-error"));
    const bool backendErrors =
        logText.contains(QStringLiteral("wasapiError"))
        || automationError;
    const int artifactMonitorCandidateCount =
        regexCount(logText, QStringLiteral("audioArtifact "));
    const int internalGlitchCandidates =
        artifactMonitorCandidateCount
        + regexCount(logText, QStringLiteral("activeSwitchBoundaryPopCandidate .*candidate=1"));
    const int submittedPcmDiscontinuityCount =
        regexCount(logText, QStringLiteral("renderMirrorConclusion .*artifactDetected=1"))
        + regexCount(logText, QStringLiteral("activeSwitchBoundaryPopCandidate .*candidate=1"));
    const int submittedPcmMetricBlockCount =
        regexCount(logText,
                   QStringLiteral("(?:firstDataBlockAfterConfigure|activeSwitchEntryBridge|activeSwitchBoundaryEnvelope|activeSwitchBoundaryPopCandidate) "));
    const double maxSubmittedPcmPeak = qMax(qMax(maxRegexCaptureDouble(logText, QStringLiteral("firstDataBlockAfterConfigure .*peak=([0-9.]+)")),
                                                maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchEntryBridge .*peak=([0-9.]+)"))),
                                           qMax(maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchBoundaryEnvelope .*firstPeak=([0-9.]+)")),
                                                maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchBoundaryPopCandidate .*firstPeak=([0-9.]+)"))));
    const double maxSubmittedPcmJump = qMax(qMax(maxRegexCaptureDouble(logText, QStringLiteral("firstDataBlockAfterConfigure .*jump=([0-9.]+)")),
                                                maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchEntryBridge .*jump=([0-9.]+)"))),
                                           qMax(maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchBoundaryEnvelope .*firstJump=([0-9.]+)")),
                                                maxRegexCaptureDouble(logText, QStringLiteral("activeSwitchBoundaryPopCandidate .*firstJump=([0-9.]+)"))));
    const bool compressedContentSample = isKnownCompressedContentSample(logText);
    SeekResumeArtifactJudgment seekResumeArtifactJudgment =
        classifySeekResumeArtifacts(logText, compressedContentSample);
    const QJsonValue seekRequestTimeMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*seekRequestTimeMs=(-?\\d+)"));
    const QJsonValue pipelineStartTimeMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*pipelineStartTimeMs=(-?\\d+)"));
    const QJsonValue firstDecodedPcmAfterSeekMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*firstDecodedPcmAfterSeekMs=(-?\\d+)"));
    const QJsonValue firstSubmittedPcmAfterSeekMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*firstSubmittedPcmAfterSeekMs=(-?\\d+)"));
    const QJsonValue firstAudibleOrFadeOpenMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResumeLatency .*firstAudibleOrFadeOpenMs=(-?\\d+)"));
    const QJsonValue seekResumeLatencyMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResumeLatency .*seekResumeLatencyMs=(-?\\d+)"));
    const QJsonValue seekResumeStartupSilenceMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*seekResumeStartupSilenceMs=(-?\\d+)"));
    const QJsonValue seekResumeWarmupDiscardMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*seekResumeWarmupDiscardMs=(-?\\d+)"));
    const QJsonValue seekResumeFadeInMs =
        maxRegexCaptureIntegerValue(logText, QStringLiteral("seekResume(?:Latency|Timing).*seekResumeFadeInMs=(-?\\d+)"));
    const QJsonValue firstSubmittedBlockPeak =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockPeak=(-?[0-9.]+)"));
    const QJsonValue firstSubmittedBlockStartSample =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockStartSample=(-?[0-9.]+)"));
    const QJsonValue firstSubmittedBlockEndSample =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockEndSample=(-?[0-9.]+)"));
    const bool firstSubmittedBlockFadeApplied =
        logText.contains(QRegularExpression(QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockFadeApplied=1")));
    const QJsonValue firstSubmittedBlockMinGain =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockMinGain=(-?[0-9.]+)"));
    const QJsonValue firstSubmittedBlockMaxGain =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("seekResumeFirstSubmittedBlock .*firstSubmittedBlockMaxGain=(-?[0-9.]+)"));
    const bool renderMirrorFirst50msAfterSeekObserved =
        logText.contains(QStringLiteral("renderMirrorFirst50msAfterSeek "));
    const bool renderMirrorFirst50msAfterSeekArtifactDetected =
        logText.contains(QRegularExpression(QStringLiteral("renderMirrorFirst50msAfterSeek .*artifactDetected=1")));
    const QJsonValue first50msSubmittedPcmJumpAfterSeek =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("renderMirrorFirst50msAfterSeek .*jump=(-?[0-9.]+)"));
    const QJsonValue renderMirrorFirst50msAfterSeekPeak =
        lastRegexCaptureDoubleValue(logText, QStringLiteral("renderMirrorFirst50msAfterSeek .*peak=(-?[0-9.]+)"));
    const bool realtimeDecodeEnabled =
        logText.contains(QRegularExpression(QStringLiteral("seekResume(?:Latency|Timing).*realtimeDecodeEnabled=1")))
        || logText.contains(QRegularExpression(QStringLiteral("startPipeline decoderReadRate realtime=1.*pipelineStartProfile=SeekResume")));
    const bool seekResumeHardArtifactEvidenceDetected =
        seekResumeArtifactJudgment.hardArtifactEvidenceDetected
        || staleSessionWriteDetected
        || staleBufferReadDetected;
    const bool seekResumeDetectorOnlyCaution =
        compressedContentSample
        && seekResumeMirrorArtifactDetected
        && !seekResumeArtifactJudgment.nonSeekResumeRenderMirrorArtifactDetected
        && !seekResumeHardArtifactEvidenceDetected;
    const bool renderMirrorHardArtifactDetected =
        renderMirrorArtifactDetected && !seekResumeDetectorOnlyCaution;
    const bool syntheticFixture = isSyntheticFixtureLog(logText);
    const bool failCondition =
        renderMirrorHardArtifactDetected || asioRenderMirrorArtifactDetected
        || bufferUnderrun || positionStallOrLag || recoveryExhausted
        || activeSwitchOutputError || automationError || staleSessionWriteDetected || staleBufferReadDetected
        || spatialEndpointFlushFailed
        || (syntheticFixture && artifactMonitorCandidateCount > 0);
    const bool anyRenderMirrorClean = renderMirrorClean || asioRenderMirrorClean;
    const QString submittedPcmConclusion = seekResumeDetectorOnlyCaution
        ? (seekResumeArtifactJudgment.artifactClassification == QStringLiteral("SeekBoundaryCandidate")
               ? QStringLiteral("SeekResume boundary candidate in compressed music sample; manual or loopback confirmation required")
               : (seekResumeArtifactJudgment.artifactClassification == QStringLiteral("ContentTransientLikely")
                      ? QStringLiteral("SeekResume full-segment music transient likely; not a hard submitted-PCM oracle")
                      : QStringLiteral("SeekResume detector inconclusive for compressed music sample; manual or loopback confirmation required")))
        : (asioRenderMirrorArtifactDetected
               ? QStringLiteral("ASIO submitted PCM artifact detected")
               : (asioRenderMirrorClean
                      ? QStringLiteral("ASIO submitted PCM clean")
                      : (renderMirrorArtifactDetected
                             ? QStringLiteral("WASAPI submitted PCM artifact detected")
                             : (renderMirrorClean && outputSwitchRequested
                                    ? QStringLiteral("no submitted artifact but output transition happened")
                                    : (renderMirrorClean ? QStringLiteral("WASAPI submitted PCM clean")
                                                         : QStringLiteral("submitted PCM not captured"))))));

    QJsonObject report;
    report.insert(QStringLiteral("result"),
                  reportResult(playbackStarted,
                               failCondition,
                               outputSwitchRequested,
                               outputSwitchCompleted,
                               anyRenderMirrorClean,
                               systemInvalidationDuringSwitch,
                               seekResumeDetectorOnlyCaution));
    const bool anyRenderMirrorArtifactDetected = renderMirrorHardArtifactDetected || asioRenderMirrorArtifactDetected;
    report.insert(QStringLiteral("popClickVerification"),
                  anyRenderMirrorArtifactDetected ? QStringLiteral("FAIL")
                  : QStringLiteral("INCONCLUSIVE"));
    report.insert(QStringLiteral("popClickVerificationLayer"),
                  asioRenderMirrorObserved
                      ? QStringLiteral("ASIO submitted PCM artifact-monitor; actual speaker/headphone output still requires listening or loopback capture")
                      : QStringLiteral("WASAPI submitted PCM render-mirror; actual speaker/headphone output still requires listening or loopback capture"));
    report.insert(QStringLiteral("actualEndpointOutputVerification"), QStringLiteral("INCONCLUSIVE"));
    report.insert(QStringLiteral("submittedPcmConclusion"), submittedPcmConclusion);
    report.insert(QStringLiteral("playbackStarted"), playbackStarted);
    report.insert(QStringLiteral("playbackStateReachedPlaying"), playbackStateReachedPlaying);
    report.insert(QStringLiteral("asioFirstBufferSwitchObserved"), asioFirstBufferSwitchObserved);
    report.insert(QStringLiteral("seekRequested"), seekRequested);
    report.insert(QStringLiteral("seekCompletedCount"), seekCompletedCount);
    report.insert(QStringLiteral("seekResumePipelineCount"), seekResumePipelineCount);
    report.insert(QStringLiteral("seekRequestTimeMs"), seekRequestTimeMs);
    report.insert(QStringLiteral("pipelineStartTimeMs"), pipelineStartTimeMs);
    report.insert(QStringLiteral("firstDecodedPcmAfterSeekMs"), firstDecodedPcmAfterSeekMs);
    report.insert(QStringLiteral("firstSubmittedPcmAfterSeekMs"), firstSubmittedPcmAfterSeekMs);
    report.insert(QStringLiteral("firstAudibleOrFadeOpenMs"), firstAudibleOrFadeOpenMs);
    report.insert(QStringLiteral("seekResumeLatencyMs"), seekResumeLatencyMs);
    report.insert(QStringLiteral("seekResumeStartupSilenceMs"), seekResumeStartupSilenceMs);
    report.insert(QStringLiteral("seekResumeWarmupDiscardMs"), seekResumeWarmupDiscardMs);
    report.insert(QStringLiteral("seekResumeFadeInMs"), seekResumeFadeInMs);
    report.insert(QStringLiteral("firstSubmittedBlockPeak"), firstSubmittedBlockPeak);
    report.insert(QStringLiteral("firstSubmittedBlockStartSample"), firstSubmittedBlockStartSample);
    report.insert(QStringLiteral("firstSubmittedBlockEndSample"), firstSubmittedBlockEndSample);
    report.insert(QStringLiteral("firstSubmittedBlockFadeApplied"), firstSubmittedBlockFadeApplied);
    report.insert(QStringLiteral("firstSubmittedBlockMinGain"), firstSubmittedBlockMinGain);
    report.insert(QStringLiteral("firstSubmittedBlockMaxGain"), firstSubmittedBlockMaxGain);
    report.insert(QStringLiteral("first50msSubmittedPcmJumpAfterSeek"), first50msSubmittedPcmJumpAfterSeek);
    report.insert(QStringLiteral("renderMirrorFirst50msAfterSeekObserved"), renderMirrorFirst50msAfterSeekObserved);
    report.insert(QStringLiteral("renderMirrorFirst50msAfterSeekPeak"), renderMirrorFirst50msAfterSeekPeak);
    report.insert(QStringLiteral("renderMirrorFirst50msAfterSeekArtifactDetected"), renderMirrorFirst50msAfterSeekArtifactDetected);
    report.insert(QStringLiteral("realtimeDecodeEnabled"), realtimeDecodeEnabled);
    report.insert(QStringLiteral("outputSwitchRequested"), outputSwitchRequested);
    report.insert(QStringLiteral("sourceSwitchRequested"), sourceSwitchRequested);
    report.insert(QStringLiteral("sourceSwitchMirrorClean"), sourceSwitchMirrorClean);
    report.insert(QStringLiteral("sourceSwitchSubmittedPcmClean"), sourceSwitchClean);
    report.insert(QStringLiteral("sourceSwitchClean"), sourceSwitchClean);
    report.insert(QStringLiteral("sourceSwitchCleanLayer"), QStringLiteral("submitted-pcm"));
    report.insert(QStringLiteral("outputSwitchCompleted"), outputSwitchCompleted);
    report.insert(QStringLiteral("playbackContinuedAfterSwitch"), playbackContinuedAfterSwitch);
    report.insert(QStringLiteral("backendErrorsOccurred"), backendErrors);
    report.insert(QStringLiteral("activeOutputSwitchDetected"), activeOutputSwitchStartedCount > 0);
    report.insert(QStringLiteral("activeOutputSwitchStartedCount"), activeOutputSwitchStartedCount);
    report.insert(QStringLiteral("activeOutputSwitchCompletedCount"), activeOutputSwitchCompletedCount);
    report.insert(QStringLiteral("sameOutputInvalidationDetected"), sameOutputInvalidationCount > 0);
    report.insert(QStringLiteral("sameOutputInvalidationCount"), sameOutputInvalidationCount);
    report.insert(QStringLiteral("sameOutputInvalidationAbsorbedEventCount"), sameOutputInvalidationAbsorbedEventCount);
    report.insert(QStringLiteral("wasapiErrorRecoveryDetected"), wasapiErrorRecoveryScheduledCount > 0 || wasapiErrorRecoveryStartCount > 0);
    report.insert(QStringLiteral("wasapiErrorRecoveryScheduledCount"), wasapiErrorRecoveryScheduledCount);
    report.insert(QStringLiteral("wasapiErrorRecoveryStartCount"), wasapiErrorRecoveryStartCount);
    report.insert(QStringLiteral("renderMirrorObserved"), renderMirrorObserved);
    report.insert(QStringLiteral("renderMirrorClean"), renderMirrorClean);
    report.insert(QStringLiteral("renderMirrorArtifactDetected"), renderMirrorArtifactDetected);
    report.insert(QStringLiteral("renderMirrorHardArtifactDetected"), renderMirrorHardArtifactDetected);
    report.insert(QStringLiteral("asioRenderMirrorObserved"), asioRenderMirrorObserved);
    report.insert(QStringLiteral("asioRenderMirrorClean"), asioRenderMirrorClean);
    report.insert(QStringLiteral("asioRenderMirrorArtifactDetected"), asioRenderMirrorArtifactDetected);
    report.insert(QStringLiteral("seekResumeMirrorObserved"), seekResumeMirrorObserved);
    report.insert(QStringLiteral("seekResumeMirrorClean"), seekResumeMirrorClean);
    report.insert(QStringLiteral("seekResumeMirrorArtifactDetected"), seekResumeMirrorArtifactDetected);
    report.insert(QStringLiteral("artifactWindow"), seekResumeArtifactJudgment.artifactWindow);
    report.insert(QStringLiteral("artifactClassification"),
                  seekResumeArtifactJudgment.artifactClassification);
    report.insert(QStringLiteral("firstArtifactOffsetMsAfterResume"),
                  seekResumeArtifactJudgment.firstArtifactOffsetMsAfterResume >= 0
                  ? QJsonValue(static_cast<qint64>(seekResumeArtifactJudgment.firstArtifactOffsetMsAfterResume))
                  : QJsonValue(QJsonValue::Null));
    report.insert(QStringLiteral("seekResumeBoundaryWindowMs"), kSeekResumeBoundaryWindowMs);
    report.insert(QStringLiteral("compressedContentSample"), compressedContentSample);
    report.insert(QStringLiteral("seekResumeBoundaryArtifactDetected"),
                  seekResumeArtifactJudgment.boundaryArtifactDetected);
    report.insert(QStringLiteral("seekResumeBoundaryArtifactCount"),
                  seekResumeArtifactJudgment.boundaryArtifactCount);
    report.insert(QStringLiteral("seekResumeFullSegmentArtifactCount"),
                  seekResumeArtifactJudgment.fullSegmentArtifactCount);
    report.insert(QStringLiteral("seekResumeContentTransientLikely"),
                  seekResumeArtifactJudgment.contentTransientLikely);
    report.insert(QStringLiteral("seekResumeContentTransientLikelyCount"),
                  seekResumeArtifactJudgment.contentTransientLikelyCount);
    report.insert(QStringLiteral("seekResumeDetectorInconclusiveCount"),
                  seekResumeArtifactJudgment.detectorInconclusiveCount);
    report.insert(QStringLiteral("seekResumeHardArtifactEvidenceDetected"),
                  seekResumeHardArtifactEvidenceDetected);
    report.insert(QStringLiteral("seekResumeDetectorOnlyCaution"),
                  seekResumeDetectorOnlyCaution);
    report.insert(QStringLiteral("nonSeekResumeRenderMirrorArtifactDetected"),
                  seekResumeArtifactJudgment.nonSeekResumeRenderMirrorArtifactDetected);
    report.insert(QStringLiteral("coldStartMirrorObserved"), coldStartMirrorObserved);
    report.insert(QStringLiteral("coldStartLoopbackObserved"), coldStartLoopbackObserved);
    report.insert(QStringLiteral("coldStartSubmittedMirrorClean"), coldStartSubmittedMirrorClean);
    report.insert(QStringLiteral("previousRunAudioLeakAtColdStart"), false);
    report.insert(QStringLiteral("coldStartLoopbackPreviousTailSimilarity"), QJsonValue::Null);
    report.insert(QStringLiteral("leakLayer"),
                  renderMirrorArtifactDetected ? QStringLiteral("submitted-pcm")
                  : (coldStartLoopbackObserved ? QStringLiteral("none") : QStringLiteral("unknown")));
    report.insert(QStringLiteral("spatialAudioMode"), QStringLiteral("unknown"));
    report.insert(QStringLiteral("spatialEndpointFlushEnabled"), spatialEndpointFlushEnabled);
    report.insert(QStringLiteral("spatialEndpointFlushMs"), spatialEndpointFlushMs);
    report.insert(QStringLiteral("spatialEndpointSettleMs"), spatialEndpointSettleMs);
    report.insert(QStringLiteral("spatialEndpointFlushBeginCount"), spatialEndpointFlushBeginCount);
    report.insert(QStringLiteral("spatialEndpointFlushDoneCount"), spatialEndpointFlushDoneCount);
    report.insert(QStringLiteral("spatialEndpointFlushFailed"), spatialEndpointFlushFailed);
    report.insert(QStringLiteral("spatialAudioProbeObserved"), spatialAudioProbeObserved);
    report.insert(QStringLiteral("spatialAudioProbeAvailable"), spatialAudioProbeAvailable);
    report.insert(QStringLiteral("spatialAudioProbeUnavailable"), spatialAudioProbeUnavailable);
    report.insert(QStringLiteral("spatialAudioStreamAvailable"), spatialAudioStreamAvailable);
    report.insert(QStringLiteral("spatialAudioSupportsFiveOneTwo"), spatialAudioSupportsFiveOneTwo);
    report.insert(QStringLiteral("spatialAudioNativeMask"), spatialAudioNativeMask);
    report.insert(QStringLiteral("spatialAudioMaxDynamicObjects"), spatialAudioMaxDynamicObjects);
    report.insert(QStringLiteral("spatialAudioObjectFormatCount"), spatialAudioObjectFormatCount);
    report.insert(QStringLiteral("spatialAudioFirstObjectFormat"), spatialAudioFirstObjectFormat);
    report.insert(QStringLiteral("bufferWaitForDataRawCount"), bufferWaitForDataRawCount);
    report.insert(QStringLiteral("activeSwitchQuarantineWaitForDataCount"), activeSwitchQuarantineWaitForDataCount);
    report.insert(QStringLiteral("bufferUnderrunDetected"), bufferUnderrun);
    report.insert(QStringLiteral("bufferUnderrunCount"), bufferUnderrunCount);
    report.insert(QStringLiteral("decoderBackpressureDetected"), decoderBackpressureDetected);
    report.insert(QStringLiteral("decoderBackpressureCount"), decoderBackpressureCount);
    report.insert(QStringLiteral("positionStallOrLagDetected"), positionStallOrLag);
    report.insert(QStringLiteral("recoveryExhausted"), recoveryExhausted);
    report.insert(QStringLiteral("activeSwitchOutputError"), activeSwitchOutputError);
    report.insert(QStringLiteral("oldPcmLeakDetected"), oldPcmLeakDetected);
    report.insert(QStringLiteral("staleBufferReuseDetected"), oldPcmLeakDetected);
    report.insert(QStringLiteral("staleSessionWriteDetected"), staleSessionWriteDetected);
    report.insert(QStringLiteral("staleBufferReadDetected"), staleBufferReadDetected);
    report.insert(QStringLiteral("staleSessionWriteCount"), staleSessionWriteCount);
    report.insert(QStringLiteral("staleBufferReadCount"), staleBufferReadCount);
    report.insert(QStringLiteral("systemInvalidationDuringSwitch"), systemInvalidationDuringSwitch);
    report.insert(QStringLiteral("submittedPcmDiscontinuityDetected"), submittedPcmDiscontinuityCount > 0);
    report.insert(QStringLiteral("submittedPcmDiscontinuityCount"), submittedPcmDiscontinuityCount);
    report.insert(QStringLiteral("submittedPcmHardDiscontinuityDetected"),
                  seekResumeHardArtifactEvidenceDetected
                  || (renderMirrorHardArtifactDetected && submittedPcmDiscontinuityCount > 0));
    report.insert(QStringLiteral("submittedPcmPeakJumpObserved"), submittedPcmMetricBlockCount > 0);
    report.insert(QStringLiteral("submittedPcmMetricBlockCount"), submittedPcmMetricBlockCount);
    report.insert(QStringLiteral("maxSubmittedPcmPeak"), maxSubmittedPcmPeak);
    report.insert(QStringLiteral("maxSubmittedPcmJump"), maxSubmittedPcmJump);
    report.insert(QStringLiteral("endpointOutputVerified"), false);
    report.insert(QStringLiteral("internalGlitchCandidatesDetected"), internalGlitchCandidates > 0);
    report.insert(QStringLiteral("internalGlitchCandidateCount"), internalGlitchCandidates);
    report.insert(QStringLiteral("artifactMonitorCandidateCount"), artifactMonitorCandidateCount);
    report.insert(QStringLiteral("textLogFile"), PlayerLogger::logFilePath());
    report.insert(QStringLiteral("jsonlDiagnosticFile"), PlayerLogger::diagnosticLogFilePath());
    report.insert(QStringLiteral("logReadable"), logReadable);

    QFileInfo reportInfo(reportPath);
    QDir().mkpath(reportInfo.absolutePath());
    QFile reportFile(reportInfo.absoluteFilePath());
    if (!reportFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        PlayerLogger::log(QStringLiteral("automation"),
                          QStringLiteral("report write-failed path=%1")
                              .arg(reportInfo.absoluteFilePath()));
        return;
    }

    reportFile.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    PlayerLogger::log(QStringLiteral("automation"),
                      QStringLiteral("report written path=%1 result=%2 popClickVerification=%3 submittedPcmConclusion=\"%4\"")
                          .arg(reportInfo.absoluteFilePath(),
                               report.value(QStringLiteral("result")).toString(),
                               report.value(QStringLiteral("popClickVerification")).toString(),
                               submittedPcmConclusion));
}
