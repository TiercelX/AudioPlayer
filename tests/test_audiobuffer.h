#ifndef TEST_AUDIOBUFFER_H
#define TEST_AUDIOBUFFER_H

#include <QObject>

class TestAudioBuffer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testInitialState();
    void testAppendAndRead();
    void testAppendExceedsMaxSize();
    void testRingBufferWrapAround();
    void testClear();
    void testSetOwner();
    void testMatchesOwner();
    void testAppendForOwnerMatching();
    void testAppendForOwnerStale();
    void testReadForOwnerMatching();
    void testReadForOwnerStale();
    void testDiscardWrites();
    void testDiscardPendingData();
    void testEndOfStream();
    void testIsSequential();
    void testWritableBytes();
    void testBufferedBytes();
    void testIsEmpty();
};

#endif // TEST_AUDIOBUFFER_H
