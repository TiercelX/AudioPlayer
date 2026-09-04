#ifndef DIAGNOSTICSDASHBOARD_H
#define DIAGNOSTICSDASHBOARD_H

#include "diagnosticsservice.h"

#include <QDockWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QWidget>

class DiagnosticsDashboard : public QDockWidget
{
    Q_OBJECT

public:
    explicit DiagnosticsDashboard(QWidget *parent = nullptr);
    ~DiagnosticsDashboard() override;

    void setDiagnosticsService(DiagnosticsService *service);
    DiagnosticsService *diagnosticsService() const;

    void setRefreshRate(int fps);
    int refreshRate() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onDiagnosticsUpdated(const DiagnosticsSnapshot &snapshot);
    void onAudioLevelsUpdated(const AudioLevelMetrics &levels);

private:
    void setupUi();
    QWidget *createAudioLevelsPanel();
    QWidget *createBufferPanel();
    QWidget *createLatencyPanel();
    QWidget *createSystemPanel();
    void updateLevelBar(QProgressBar *bar, qreal level);
    QString formatBytes(qint64 bytes) const;
    QString formatLatency(qreal ms) const;

    DiagnosticsService *m_service = nullptr;

    QProgressBar *m_leftLevelBar = nullptr;
    QProgressBar *m_rightLevelBar = nullptr;
    QProgressBar *m_peakLevelBar = nullptr;
    QLabel *m_rmsLabel = nullptr;

    QProgressBar *m_bufferUsageBar = nullptr;
    QLabel *m_bufferUsedLabel = nullptr;
    QLabel *m_bufferCapacityLabel = nullptr;

    QLabel *m_decodeLatencyLabel = nullptr;
    QLabel *m_outputLatencyLabel = nullptr;
    QLabel *m_totalLatencyLabel = nullptr;

    QLabel *m_cpuUsageLabel = nullptr;
    QProgressBar *m_cpuUsageBar = nullptr;
    QLabel *m_memoryUsageLabel = nullptr;
    QLabel *m_threadCountLabel = nullptr;
};

#endif // DIAGNOSTICSDASHBOARD_H
