#ifndef TEST_ALSA_LOGIC_H
#define TEST_ALSA_LOGIC_H

#include <QObject>

class TestAlsaLogic : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // rawInputFormatForPath
    void testRawFormatTruehd();
    void testRawFormatMlp();
    void testRawFormatThd();
    void testRawFormatEac3();
    void testRawFormatEb3();
    void testRawFormatAc3();
    void testRawFormatFlac();
    void testRawFormatEmpty();
    void testCaseInsensitive();

    // startupThresholdBytes
    void testThresholdNormalStart();
    void testThresholdSeekResume();
    void testThresholdInvalidFormat();
    void testThresholdMinimumFloor();
};

#endif // TEST_ALSA_LOGIC_H
