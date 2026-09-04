#ifndef PCMSEEKCACHE_H
#define PCMSEEKCACHE_H

#include "ffmpegpcmshared.h"

#include <QByteArray>
#include <QFile>
#include <QMutex>
#include <QString>
#include <QVector>

class PcmSeekCache
{
public:
    struct Segment {
        qint64 positionMs = 0;
        qint64 durationMs = 0;
        qsizetype byteOffset = 0;
        qsizetype byteSize = 0;
        QByteArray data;
    };

    struct Hit {
        qint64 positionMs = 0;
        qint64 durationMs = 0;
        qsizetype byteOffset = 0;
        qsizetype byteSize = 0;
        bool valid = false;
    };

    PcmSeekCache();
    ~PcmSeekCache();

    PcmSeekCache(const PcmSeekCache &) = delete;
    PcmSeekCache &operator=(const PcmSeekCache &) = delete;

    bool initialize(const QString &sourcePath,
                    const PcmStreamFormat &format,
                    const QString &cacheRoot,
                    int maxCacheMiB = 256,
                    int maxAgeMinutes = 30);
    void clear();
    bool isInitialized() const;

    void writeSegment(qint64 positionMs, const QByteArray &pcmData, const PcmStreamFormat &format);
    Hit findHit(qint64 targetMs, qint64 toleranceMs = 1000) const;
    QByteArray readSegment(const Hit &hit) const;

    qint64 totalCachedBytes() const;
    int segmentCount() const;

private:
    enum class StorageMode { Memory, Disk, Disabled };

    struct CacheKey {
        QString sourcePath;
        int sampleRate = 0;
        int channelCount = 0;
        int bitsPerSample = 0;
        QString channelLayout;

        bool operator==(const CacheKey &other) const;
        QString toHash() const;
    };

    bool detectStorageMode(int requestedMiB);
    bool initDiskCache(const QString &cacheRoot, const QString &keyHash);
    void pruneByAge(int maxAgeMinutes);
    void pruneByBytes(qsizetype maxBytes);

    mutable QMutex m_mutex;
    CacheKey m_key;
    QVector<Segment> m_segments;

    StorageMode m_mode = StorageMode::Disabled;
    qsizetype m_memoryCapacity = 0;
    bool m_memoryWriteLimitReached = false;

    QString m_diskPath;
    mutable QFile m_diskFile;

    qsizetype m_totalCachedBytes = 0;
    bool m_initialized = false;
};

#endif // PCMSEEKCACHE_H
