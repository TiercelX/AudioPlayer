#include "pcmseekcache.h"

#include "playerlogger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>

namespace {

constexpr qsizetype kDefaultMemoryCapacity = 256 * 1024 * 1024;
constexpr qint64 kBytesPerMiB = 1024 * 1024;
constexpr qint64 kMaxCoalescedSegmentDurationMs = 1000;

qint64 availablePhysicalMemoryBytes()
{
#ifdef Q_OS_WIN
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        return static_cast<qint64>(memInfo.ullAvailPhys);
    }
#endif
    return 0;
}

} // namespace

PcmSeekCache::PcmSeekCache() = default;

PcmSeekCache::~PcmSeekCache()
{
    clear();
}

bool PcmSeekCache::CacheKey::operator==(const CacheKey &other) const
{
    return sourcePath == other.sourcePath
        && sampleRate == other.sampleRate
        && channelCount == other.channelCount
        && bitsPerSample == other.bitsPerSample
        && channelLayout == other.channelLayout;
}

QString PcmSeekCache::CacheKey::toHash() const
{
    const QByteArray cacheKey = sourcePath.toUtf8()
        + '|' + QByteArray::number(sampleRate)
        + '|' + QByteArray::number(channelCount)
        + '|' + QByteArray::number(bitsPerSample)
        + '|' + channelLayout.toUtf8()
        + '|' + QByteArray("pcm-seek");
    return QString::fromLatin1(
        QCryptographicHash::hash(cacheKey, QCryptographicHash::Sha1).toHex());
}

bool PcmSeekCache::detectStorageMode(int requestedMiB)
{
    const qint64 available = availablePhysicalMemoryBytes();
    const qint64 requested = static_cast<qint64>(requestedMiB) * kBytesPerMiB;

    if (available <= 0 || available > requested + 128 * kBytesPerMiB) {
        m_mode = StorageMode::Memory;
        m_memoryCapacity = requested > 0 ? requested : kDefaultMemoryCapacity;
        m_memoryWriteLimitReached = false;
        PlayerLogger::log(QStringLiteral("pcmseekcache"),
                          QStringLiteral("storageMode=memory capacityMiB=%1 availableMiB=%2")
                              .arg(m_memoryCapacity / kBytesPerMiB)
                              .arg(available / kBytesPerMiB));
        return true;
    }

    m_mode = StorageMode::Disk;
    PlayerLogger::log(QStringLiteral("pcmseekcache"),
                      QStringLiteral("storageMode=disk reason=lowMemory availableMiB=%1 requestedMiB=%2")
                          .arg(available / kBytesPerMiB)
                          .arg(requestedMiB));
    return true;
}

bool PcmSeekCache::initDiskCache(const QString &cacheRoot, const QString &keyHash)
{
    const QString seekCacheDir = QDir(cacheRoot).filePath(QStringLiteral("pcm-seek"));
    QDir dir(seekCacheDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        PlayerLogger::log(QStringLiteral("pcmseekcache"),
                          QStringLiteral("initDiskCache failed: cannot create dir=%1").arg(seekCacheDir));
        m_mode = StorageMode::Disabled;
        return false;
    }

    m_diskPath = dir.filePath(keyHash + QStringLiteral(".pcm"));
    m_diskFile.setFileName(m_diskPath);

    if (m_diskFile.exists()) {
        PlayerLogger::log(QStringLiteral("pcmseekcache"),
                          QStringLiteral("initDiskCache existing file=%1 sizeMiB=%2")
                              .arg(m_diskPath)
                              .arg(m_diskFile.size() / kBytesPerMiB));
    }

    return true;
}

bool PcmSeekCache::initialize(const QString &sourcePath,
                               const PcmStreamFormat &format,
                               const QString &cacheRoot,
                               int maxCacheMiB,
                               int maxAgeMinutes)
{
    QMutexLocker locker(&m_mutex);

    if (m_initialized) {
        clear();
    }

    if (maxCacheMiB <= 0) {
        m_mode = StorageMode::Disabled;
        PlayerLogger::log(QStringLiteral("pcmseekcache"), QStringLiteral("disabled maxCacheMiB=0"));
        return false;
    }

    m_key = {sourcePath, format.sampleRate, format.channelCount, format.bitsPerSample(), format.channelLayout};
    const QString keyHash = m_key.toHash();

    if (!detectStorageMode(maxCacheMiB)) {
        return false;
    }

    if (m_mode == StorageMode::Disk) {
        if (!initDiskCache(cacheRoot, keyHash)) {
            return false;
        }
    }

    m_initialized = true;
    return true;
}

void PcmSeekCache::clear()
{
    QMutexLocker locker(&m_mutex);

    m_segments.clear();
    m_memoryWriteLimitReached = false;
    m_totalCachedBytes = 0;

    if (m_diskFile.isOpen()) {
        m_diskFile.close();
    }
    if (!m_diskPath.isEmpty()) {
        QFile::remove(m_diskPath);
        m_diskPath.clear();
    }

    m_initialized = false;
    m_mode = StorageMode::Disabled;
}

bool PcmSeekCache::isInitialized() const
{
    QMutexLocker locker(&m_mutex);
    return m_initialized;
}

void PcmSeekCache::writeSegment(qint64 positionMs, const QByteArray &pcmData, const PcmStreamFormat &format)
{
    QMutexLocker locker(&m_mutex);

    if (!m_initialized || m_mode == StorageMode::Disabled || pcmData.isEmpty()) {
        return;
    }

    const qint64 bytesPerSecond = static_cast<qint64>(format.bytesPerFrame()) * format.sampleRate;
    if (bytesPerSecond <= 0) {
        return;
    }
    const qint64 durationMs = qMax<qint64>(
        1,
        (static_cast<qint64>(pcmData.size()) * 1000 + bytesPerSecond - 1)
            / bytesPerSecond);
    const qsizetype maxCoalescedSegmentBytes =
        qMax<qsizetype>(pcmData.size(),
                        static_cast<qsizetype>(bytesPerSecond * kMaxCoalescedSegmentDurationMs / 1000));
    auto canCoalesceWithLast = [&]() {
        if (m_segments.isEmpty()) {
            return false;
        }
        const Segment &last = m_segments.constLast();
        const qint64 lastEndMs = last.positionMs + last.durationMs;
        return positionMs >= last.positionMs
               && positionMs <= lastEndMs + 2
               && last.byteSize + pcmData.size() <= maxCoalescedSegmentBytes;
    };

    if (m_mode == StorageMode::Memory) {
        if (pcmData.size() > m_memoryCapacity) {
            if (!m_memoryWriteLimitReached) {
                m_memoryWriteLimitReached = true;
                PlayerLogger::log(QStringLiteral("pcmseekcache"),
                                  QStringLiteral("memory write limit reached capacityMiB=%1 cachedMiB=%2 "
                                                 "segmentKiB=%3; skipping oversized segment")
                                      .arg(m_memoryCapacity / kBytesPerMiB)
                                      .arg(m_totalCachedBytes / kBytesPerMiB)
                                      .arg(pcmData.size() / 1024));
            }
            return;
        }

        if (canCoalesceWithLast()) {
            Segment &last = m_segments.last();
            last.data.append(pcmData);
            last.byteSize += pcmData.size();
            last.durationMs = qMax<qint64>(
                last.durationMs,
                (static_cast<qint64>(last.byteSize) * 1000 + bytesPerSecond - 1)
                    / bytesPerSecond);
        } else {
            m_segments.append({positionMs, durationMs, 0, pcmData.size(), pcmData});
        }
        m_totalCachedBytes += pcmData.size();
        pruneByBytes(m_memoryCapacity);
    } else if (m_mode == StorageMode::Disk) {
        if (!m_diskFile.isOpen()) {
            if (!m_diskFile.open(QIODevice::ReadWrite)) {
                PlayerLogger::log(QStringLiteral("pcmseekcache"),
                                  QStringLiteral("writeSegment failed: cannot open disk file=%1").arg(m_diskPath));
                m_mode = StorageMode::Disabled;
                return;
            }
        }

        const qsizetype writePos = m_diskFile.size();
        m_diskFile.seek(writePos);
        const qint64 written = m_diskFile.write(pcmData);
        if (written != pcmData.size()) {
            PlayerLogger::log(QStringLiteral("pcmseekcache"),
                              QStringLiteral("writeSegment failed: disk write incomplete written=%1 expected=%2")
                                  .arg(written).arg(pcmData.size()));
            return;
        }
        m_diskFile.flush();

        if (canCoalesceWithLast()
            && !m_segments.isEmpty()
            && m_segments.constLast().byteOffset + m_segments.constLast().byteSize == writePos) {
            Segment &last = m_segments.last();
            last.byteSize += pcmData.size();
            last.durationMs = qMax<qint64>(
                last.durationMs,
                (static_cast<qint64>(last.byteSize) * 1000 + bytesPerSecond - 1)
                    / bytesPerSecond);
        } else {
            m_segments.append({positionMs, durationMs, writePos, pcmData.size(), {}});
        }
        m_totalCachedBytes += pcmData.size();
    }
}

PcmSeekCache::Hit PcmSeekCache::findHit(qint64 targetMs, qint64 toleranceMs) const
{
    QMutexLocker locker(&m_mutex);

    if (!m_initialized || m_segments.isEmpty()) {
        return {};
    }

    for (int i = m_segments.size() - 1; i >= 0; --i) {
        const Segment &seg = m_segments[i];
        if (seg.positionMs <= targetMs && targetMs < seg.positionMs + seg.durationMs + toleranceMs) {
            return {seg.positionMs, seg.durationMs, seg.byteOffset, seg.byteSize, true};
        }
        if (seg.positionMs + seg.durationMs + toleranceMs < targetMs) {
            break;
        }
    }

    return {};
}

QByteArray PcmSeekCache::readSegment(const Hit &hit) const
{
    QMutexLocker locker(&m_mutex);

    if (!hit.valid || hit.byteSize <= 0) {
        return {};
    }

    if (m_mode == StorageMode::Memory) {
        for (const auto &segment : m_segments) {
            if (segment.positionMs == hit.positionMs
                && segment.durationMs == hit.durationMs
                && segment.byteSize == hit.byteSize) {
                return segment.data;
            }
        }
        return {};
    }

    if (m_mode == StorageMode::Disk) {
        if (!m_diskFile.isOpen() || !m_diskFile.isReadable()) {
            return {};
        }
        m_diskFile.seek(hit.byteOffset);
        return m_diskFile.read(hit.byteSize);
    }

    return {};
}

qint64 PcmSeekCache::totalCachedBytes() const
{
    QMutexLocker locker(&m_mutex);
    return m_totalCachedBytes;
}

int PcmSeekCache::segmentCount() const
{
    QMutexLocker locker(&m_mutex);
    return m_segments.size();
}

void PcmSeekCache::pruneByAge(int maxAgeMinutes)
{
    Q_UNUSED(maxAgeMinutes);
}

void PcmSeekCache::pruneByBytes(qsizetype maxBytes)
{
    if (m_totalCachedBytes <= maxBytes || m_segments.isEmpty()) {
        return;
    }

    qsizetype bytesToFree = m_totalCachedBytes - maxBytes;
    int segmentsToRemove = 0;

    for (const auto &seg : m_segments) {
        if (bytesToFree <= 0) {
            break;
        }
        bytesToFree -= seg.byteSize;
        ++segmentsToRemove;
    }

    if (segmentsToRemove > 0) {
        m_segments.remove(0, segmentsToRemove);

        m_totalCachedBytes = 0;
        for (const auto &seg : m_segments) {
            m_totalCachedBytes += seg.byteSize;
        }
    }
}
