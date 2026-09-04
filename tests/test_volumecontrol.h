#ifndef TEST_VOLUMECONTROL_H
#define TEST_VOLUMECONTROL_H

#include <QObject>

class TestVolumeControl : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testDefaultVolume();
    void testSetVolumeNormalRange();
    void testSetVolumeBoundaryNegative();
    void testSetVolumeBoundaryExceedsOne();
    void testSetVolumeMute();
    void testSetVolumeMultipleUpdates();
};

#endif // TEST_VOLUMECONTROL_H
