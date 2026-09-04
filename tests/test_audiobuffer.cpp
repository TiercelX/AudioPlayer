#include "test_audiobuffer.h"
#include "ffmpegpcmshared.h"

#include <QSignalSpy>
#include <QTest>

void TestAudioBuffer::initTestCase() {}
void TestAudioBuffer::cleanupTestCase() {}

void TestAudioBuffer::testInitialState()
{
    PcmStreamBuffer buf;
    QVERIFY(buf.isEmpty());
    QCOMPARE(buf.bufferedBytes(), 0);
    QCOMPARE(buf.maxSize(), 0);
    QCOMPARE(buf.ownerSessionId(), 0);
    QCOMPARE(buf.bufferGeneration(), quint64(0));
    QVERIFY(!buf.endOfStream());
    QVERIFY(!buf.isDiscardingWrites());
    QCOMPARE(buf.discardedWriteBytes(), qint64(0));
    QVERIFY(buf.isSequential());
}

void TestAudioBuffer::testAppendAndRead()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(1024);

    QByteArray data = QByteArray::fromRawData("abcdefgh", 8);
    qint64 written = buf.append(data);
    QCOMPARE(written, qint64(8));
    QCOMPARE(buf.bufferedBytes(), qsizetype(8));
    QVERIFY(!buf.isEmpty());

    QByteArray readData = buf.readForOwner(8, 0, 0);
    QCOMPARE(readData.size(), 8);
    QCOMPARE(readData, data);
    QVERIFY(buf.isEmpty());
}

void TestAudioBuffer::testAppendExceedsMaxSize()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(4);

    QByteArray data = QByteArray::fromRawData("abcdefgh", 8);
    qint64 written = buf.append(data);
    QCOMPARE(written, qint64(4));
    QCOMPARE(buf.bufferedBytes(), qsizetype(4));

    QByteArray readData = buf.readForOwner(4, 0, 0);
    QCOMPARE(readData, QByteArray("abcd", 4));
}

void TestAudioBuffer::testRingBufferWrapAround()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(8);

    QByteArray data1 = QByteArray::fromRawData("abcdefgh", 8);
    buf.append(data1);

    QByteArray readPart = buf.readForOwner(4, 0, 0);
    QCOMPARE(readPart, QByteArray("abcd", 4));

    QByteArray data2 = QByteArray::fromRawData("ijkl", 4);
    buf.append(data2);
    QCOMPARE(buf.bufferedBytes(), qsizetype(8));

    QByteArray readRest = buf.readForOwner(8, 0, 0);
    QCOMPARE(readRest, QByteArray("efghijkl", 8));
}

void TestAudioBuffer::testClear()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);
    buf.append(QByteArray("hello", 5));
    QCOMPARE(buf.bufferedBytes(), qsizetype(5));

    buf.clear();
    QVERIFY(buf.isEmpty());
    QCOMPARE(buf.bufferedBytes(), 0);
}

void TestAudioBuffer::testSetOwner()
{
    PcmStreamBuffer buf;

    buf.setOwner(42, 7, QStringLiteral("test-source"));
    QCOMPARE(buf.ownerSessionId(), 42);
    QCOMPARE(buf.bufferGeneration(), quint64(7));
    QCOMPARE(buf.ownerSource(), QStringLiteral("test-source"));

    buf.setOwner(0, 0);
    QCOMPARE(buf.ownerSessionId(), 0);
    QCOMPARE(buf.bufferGeneration(), quint64(0));
}

void TestAudioBuffer::testMatchesOwner()
{
    PcmStreamBuffer buf;
    QVERIFY(buf.matchesOwner(1, 1));
    QVERIFY(buf.matchesOwner(999, 999));

    buf.setOwner(42, 7);
    QVERIFY(buf.matchesOwner(42, 7));
    QVERIFY(!buf.matchesOwner(42, 8));
    QVERIFY(!buf.matchesOwner(43, 7));
    QVERIFY(!buf.matchesOwner(43, 8));

    buf.setOwner(0, 0);
    QVERIFY(buf.matchesOwner(1, 1));
}

void TestAudioBuffer::testAppendForOwnerMatching()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);
    buf.setOwner(10, 3);

    QByteArray data = QByteArray::fromRawData("test", 4);
    qint64 written = buf.appendForOwner(data, 10, 3);
    QCOMPARE(written, qint64(4));
    QCOMPARE(buf.bufferedBytes(), qsizetype(4));
}

void TestAudioBuffer::testAppendForOwnerStale()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);
    buf.setOwner(10, 3);

    QByteArray data = QByteArray::fromRawData("test", 4);
    qint64 written = buf.appendForOwner(data, 10, 4);
    QCOMPARE(written, qint64(0));
    QVERIFY(buf.isEmpty());

    written = buf.appendForOwner(data, 11, 3);
    QCOMPARE(written, qint64(0));
    QVERIFY(buf.isEmpty());

    QCOMPARE(buf.discardedWriteBytes(), qint64(8));
}

void TestAudioBuffer::testReadForOwnerMatching()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);
    buf.setOwner(10, 3);

    buf.append(QByteArray("abcdefgh", 8));

    bool staleRead = false;
    QByteArray readData = buf.readForOwner(4, 10, 3, &staleRead);
    QVERIFY(!staleRead);
    QCOMPARE(readData, QByteArray("abcd", 4));
    QCOMPARE(buf.bufferedBytes(), qsizetype(4));
}

void TestAudioBuffer::testReadForOwnerStale()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);
    buf.setOwner(10, 3);

    buf.append(QByteArray("abcdefgh", 8));

    bool staleRead = false;
    QByteArray readData = buf.readForOwner(4, 10, 4, &staleRead);
    QVERIFY(staleRead);
    QVERIFY(readData.isEmpty());
    QCOMPARE(buf.bufferedBytes(), qsizetype(8));
}

void TestAudioBuffer::testDiscardWrites()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);

    buf.setDiscardWrites(true);
    QVERIFY(buf.isDiscardingWrites());

    QByteArray data = QByteArray::fromRawData("test", 4);
    qint64 written = buf.append(data);
    QCOMPARE(written, qint64(0));
    QVERIFY(buf.isEmpty());
    QCOMPARE(buf.discardedWriteBytes(), qint64(4));

    buf.append(data);
    QCOMPARE(buf.discardedWriteBytes(), qint64(8));

    buf.setDiscardWrites(false);
    QVERIFY(!buf.isDiscardingWrites());
    QCOMPARE(buf.discardedWriteBytes(), qint64(0));

    written = buf.append(data);
    QCOMPARE(written, qint64(4));
}

void TestAudioBuffer::testDiscardPendingData()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);

    buf.append(QByteArray("abcdefgh", 8));
    QCOMPARE(buf.bufferedBytes(), qsizetype(8));

    qsizetype discarded = buf.discardPendingData();
    QCOMPARE(discarded, qsizetype(8));
    QVERIFY(buf.isEmpty());
    QVERIFY(buf.isDiscardingWrites());
}

void TestAudioBuffer::testEndOfStream()
{
    PcmStreamBuffer buf;
    QVERIFY(!buf.endOfStream());

    buf.setEndOfStream(true);
    QVERIFY(buf.endOfStream());

    buf.setEndOfStream(false);
    QVERIFY(!buf.endOfStream());
}

void TestAudioBuffer::testIsSequential()
{
    PcmStreamBuffer buf;
    QVERIFY(buf.isSequential());
}

void TestAudioBuffer::testWritableBytes()
{
    PcmStreamBuffer buf;
    QCOMPARE(buf.writableBytes(), qsizetype(0));

    buf.setMaxSize(16);
    QCOMPARE(buf.writableBytes(), qsizetype(16));

    buf.append(QByteArray("abcd", 4));
    QCOMPARE(buf.writableBytes(), qsizetype(12));

    buf.append(QByteArray("abcdefghijkl", 12));
    QCOMPARE(buf.writableBytes(), qsizetype(0));
}

void TestAudioBuffer::testBufferedBytes()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);

    QCOMPARE(buf.bufferedBytes(), qsizetype(0));

    buf.append(QByteArray("hello", 5));
    QCOMPARE(buf.bufferedBytes(), qsizetype(5));

    buf.append(QByteArray(" world", 6));
    QCOMPARE(buf.bufferedBytes(), qsizetype(11));

    buf.readForOwner(3, 0, 0);
    QCOMPARE(buf.bufferedBytes(), qsizetype(8));
}

void TestAudioBuffer::testIsEmpty()
{
    PcmStreamBuffer buf;
    buf.setMaxSize(64);

    QVERIFY(buf.isEmpty());

    buf.append(QByteArray("x", 1));
    QVERIFY(!buf.isEmpty());

    buf.readForOwner(1, 0, 0);
    QVERIFY(buf.isEmpty());
}


