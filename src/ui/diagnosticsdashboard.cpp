#include "diagnosticsdashboard.h"

#include <QCloseEvent>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

DiagnosticsDashboard::DiagnosticsDashboard(QWidget *parent)
    : QDockWidget(tr("Performance Monitor"), parent)
{
    setupUi();
    setMinimumWidth(280);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
}

DiagnosticsDashboard::~DiagnosticsDashboard() = default;

void DiagnosticsDashboard::setDiagnosticsService(DiagnosticsService *service)
{
    if (m_service == service) {
        return;
    }

    if (m_service) {
        disconnect(m_service, nullptr, this, nullptr);
    }

    m_service = service;

    if (m_service) {
        connect(m_service, &QObject::destroyed, this, [this]() {
            m_service = nullptr;
        });
        connect(m_service, &DiagnosticsService::diagnosticsUpdated,
                this, &DiagnosticsDashboard::onDiagnosticsUpdated);
        connect(m_service, &DiagnosticsService::audioLevelsUpdated,
                this, &DiagnosticsDashboard::onAudioLevelsUpdated);
    }
}

DiagnosticsService *DiagnosticsDashboard::diagnosticsService() const
{
    return m_service;
}

void DiagnosticsDashboard::setRefreshRate(int fps)
{
    if (fps > 0 && m_service) {
        m_service->setUpdateIntervalMs(1000 / fps);
    }
}

int DiagnosticsDashboard::refreshRate() const
{
    return 20;
}

void DiagnosticsDashboard::closeEvent(QCloseEvent *event)
{
    event->accept();
}

void DiagnosticsDashboard::onDiagnosticsUpdated(const DiagnosticsSnapshot &snapshot)
{
    updateLevelBar(m_peakLevelBar, snapshot.audioLevels.peakLevel);
    m_rmsLabel->setText(tr("%1").arg(snapshot.audioLevels.rmsLevel, 0, 'f', 3));

    const qint64 used = snapshot.buffer.bufferUsed;
    const qint64 capacity = snapshot.buffer.bufferCapacity;
    m_bufferUsedLabel->setText(formatBytes(used));
    m_bufferCapacityLabel->setText(formatBytes(capacity));
    if (capacity > 0) {
        m_bufferUsageBar->setValue(static_cast<int>(used * 100 / capacity));
    }

    m_decodeLatencyLabel->setText(formatLatency(snapshot.latency.decodeLatencyMs));
    m_outputLatencyLabel->setText(formatLatency(snapshot.latency.outputLatencyMs));
    m_totalLatencyLabel->setText(formatLatency(snapshot.latency.totalLatencyMs));

    m_cpuUsageLabel->setText(tr("%1%").arg(snapshot.system.cpuUsagePercent, 0, 'f', 1));
    m_cpuUsageBar->setValue(static_cast<int>(snapshot.system.cpuUsagePercent));
    m_memoryUsageLabel->setText(formatBytes(snapshot.system.memoryUsageBytes));
    m_threadCountLabel->setText(tr("%1").arg(snapshot.system.threadCount));
}

void DiagnosticsDashboard::onAudioLevelsUpdated(const AudioLevelMetrics &levels)
{
    updateLevelBar(m_leftLevelBar, levels.leftLevel);
    updateLevelBar(m_rightLevelBar, levels.rightLevel);
}

void DiagnosticsDashboard::setupUi()
{
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    layout->addWidget(createAudioLevelsPanel());
    layout->addWidget(createBufferPanel());
    layout->addWidget(createLatencyPanel());
    layout->addWidget(createSystemPanel());
    layout->addStretch();

    setWidget(content);
}

QWidget *DiagnosticsDashboard::createAudioLevelsPanel()
{
    auto *group = new QGroupBox(tr("Audio Levels"), this);
    auto *layout = new QVBoxLayout(group);

    auto addLevelRow = [&](const QString &labelText) -> QProgressBar * {
        auto *row = new QHBoxLayout();
        auto *label = new QLabel(labelText, this);
        label->setFixedWidth(60);
        auto *bar = new QProgressBar(this);
        bar->setRange(0, 1000);
        bar->setTextVisible(false);
        bar->setFixedHeight(12);
        row->addWidget(label);
        row->addWidget(bar);
        layout->addLayout(row);
        return bar;
    };

    m_leftLevelBar = addLevelRow(tr("Left"));
    m_rightLevelBar = addLevelRow(tr("Right"));
    m_peakLevelBar = addLevelRow(tr("Peak"));

    auto *rmsRow = new QHBoxLayout();
    auto *rmsTitle = new QLabel(tr("RMS:"), this);
    rmsTitle->setFixedWidth(60);
    m_rmsLabel = new QLabel(tr("0.000"), this);
    rmsRow->addWidget(rmsTitle);
    rmsRow->addWidget(m_rmsLabel);
    layout->addLayout(rmsRow);

    return group;
}

QWidget *DiagnosticsDashboard::createBufferPanel()
{
    auto *group = new QGroupBox(tr("Buffer Status"), this);
    auto *layout = new QVBoxLayout(group);

    m_bufferUsageBar = new QProgressBar(this);
    m_bufferUsageBar->setRange(0, 100);
    m_bufferUsageBar->setTextVisible(true);
    m_bufferUsageBar->setFormat(tr("%p%"));
    layout->addWidget(m_bufferUsageBar);

    auto *usedRow = new QHBoxLayout();
    auto *usedTitle = new QLabel(tr("Used:"), this);
    usedTitle->setFixedWidth(70);
    m_bufferUsedLabel = new QLabel(tr("0 B"), this);
    usedRow->addWidget(usedTitle);
    usedRow->addWidget(m_bufferUsedLabel);
    layout->addLayout(usedRow);

    auto *capRow = new QHBoxLayout();
    auto *capTitle = new QLabel(tr("Capacity:"), this);
    capTitle->setFixedWidth(70);
    m_bufferCapacityLabel = new QLabel(tr("0 B"), this);
    capRow->addWidget(capTitle);
    capRow->addWidget(m_bufferCapacityLabel);
    layout->addLayout(capRow);

    return group;
}

QWidget *DiagnosticsDashboard::createLatencyPanel()
{
    auto *group = new QGroupBox(tr("Latency"), this);
    auto *layout = new QVBoxLayout(group);

    auto addLatencyRow = [&](const QString &title) -> QLabel * {
        auto *row = new QHBoxLayout();
        auto *titleLabel = new QLabel(title, this);
        titleLabel->setFixedWidth(70);
        auto *valueLabel = new QLabel(tr("0.0 ms"), this);
        row->addWidget(titleLabel);
        row->addWidget(valueLabel);
        layout->addLayout(row);
        return valueLabel;
    };

    m_decodeLatencyLabel = addLatencyRow(tr("Decode:"));
    m_outputLatencyLabel = addLatencyRow(tr("Output:"));

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    layout->addWidget(separator);

    m_totalLatencyLabel = addLatencyRow(tr("Total:"));

    return group;
}

QWidget *DiagnosticsDashboard::createSystemPanel()
{
    auto *group = new QGroupBox(tr("System"), this);
    auto *layout = new QVBoxLayout(group);

    auto *cpuRow = new QHBoxLayout();
    auto *cpuTitle = new QLabel(tr("CPU:"), this);
    cpuTitle->setFixedWidth(70);
    m_cpuUsageLabel = new QLabel(tr("0%"), this);
    m_cpuUsageBar = new QProgressBar(this);
    m_cpuUsageBar->setRange(0, 100);
    m_cpuUsageBar->setTextVisible(false);
    m_cpuUsageBar->setFixedHeight(12);
    cpuRow->addWidget(cpuTitle);
    cpuRow->addWidget(m_cpuUsageLabel);
    cpuRow->addWidget(m_cpuUsageBar);
    layout->addLayout(cpuRow);

    auto *memRow = new QHBoxLayout();
    auto *memTitle = new QLabel(tr("Memory:"), this);
    memTitle->setFixedWidth(70);
    m_memoryUsageLabel = new QLabel(tr("0 MB"), this);
    memRow->addWidget(memTitle);
    memRow->addWidget(m_memoryUsageLabel);
    layout->addLayout(memRow);

    auto *threadRow = new QHBoxLayout();
    auto *threadTitle = new QLabel(tr("Threads:"), this);
    threadTitle->setFixedWidth(70);
    m_threadCountLabel = new QLabel(tr("0"), this);
    threadRow->addWidget(threadTitle);
    threadRow->addWidget(m_threadCountLabel);
    layout->addLayout(threadRow);

    return group;
}

void DiagnosticsDashboard::updateLevelBar(QProgressBar *bar, qreal level)
{
    if (!bar) {
        return;
    }
    const int value = static_cast<int>(qBound(0.0, level, 1.0) * 1000);
    bar->setValue(value);
}

QString DiagnosticsDashboard::formatBytes(qint64 bytes) const
{
    if (bytes < 1024) {
        return tr("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return tr("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    } else if (bytes < 1024 * 1024 * 1024) {
        return tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        return tr("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
}

QString DiagnosticsDashboard::formatLatency(qreal ms) const
{
    return tr("%1 ms").arg(ms, 0, 'f', 1);
}
