#ifndef TEST_AUDIOPLAYERFACTORY_H
#define TEST_AUDIOPLAYERFACTORY_H

#include <QObject>

class TestAudioPlayerFactory : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testSourceContextDefaultConstruction();
    void testSourceContextFieldAssignment();
    void testPlaybackPlanDefaults();
    void testPlaybackPlanSourceModeValues();
    void testBackendIdEnumValues();
};

#endif // TEST_AUDIOPLAYERFACTORY_H
