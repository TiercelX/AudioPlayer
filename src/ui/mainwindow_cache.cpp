#include "mainwindow.h"
#include "mainwindow_helpers.h"
#include "playerlogger.h"
#include "playbacksourceservice.h"
#include "ui_mainwindow.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QStatusBar>
#include <QVBoxLayout>

namespace {

using namespace MainWindowHelpers;

} // namespace

void MainWindow::openCacheSettingsDialog()
{
    PlaybackCacheSettings cacheSettings = m_playbackSourceService.cacheSettings();
    PlaybackCacheUsage usage = m_playbackSourceService.cacheUsage();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("缓存设置"));
    auto *layout = new QVBoxLayout(&dialog);

    auto *descriptionLabel = new QLabel(
        tr("缓存用于保存原始 Dolby 文件的时间轴容器、诊断/loopback 音频，以及 seek 解码 PCM 缓存。回收会在载入需要缓存的文件时执行，也可以在这里立即执行。"),
        &dialog);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);

    auto *formLayout = new QFormLayout;
    auto *directoryEdit = new QLineEdit(cacheSettings.cacheDirectory, &dialog);
    auto *browseButton = new QPushButton(tr("选择..."), &dialog);
    auto *directoryRow = new QWidget(&dialog);
    auto *directoryLayout = new QHBoxLayout(directoryRow);
    directoryLayout->setContentsMargins(0, 0, 0, 0);
    directoryLayout->addWidget(directoryEdit, 1);
    directoryLayout->addWidget(browseButton);
    formLayout->addRow(tr("缓存目录"), directoryRow);

    auto *sidecarFilesSpin = new QSpinBox(&dialog);
    sidecarFilesSpin->setRange(1, 1000);
    sidecarFilesSpin->setValue(cacheSettings.maxSidecars);
    formLayout->addRow(tr("Sidecar 最多文件数"), sidecarFilesSpin);

    auto *sidecarAgeSpin = new QSpinBox(&dialog);
    sidecarAgeSpin->setRange(1, 3650);
    sidecarAgeSpin->setSuffix(tr(" 天"));
    sidecarAgeSpin->setValue(cacheSettings.maxSidecarAgeDays);
    formLayout->addRow(tr("Sidecar 保留时间"), sidecarAgeSpin);

    auto *sidecarSizeSpin = new QSpinBox(&dialog);
    sidecarSizeSpin->setRange(1, 102400);
    sidecarSizeSpin->setSuffix(tr(" MiB"));
    sidecarSizeSpin->setValue(cacheSettings.maxSidecarMiB);
    formLayout->addRow(tr("Sidecar 总大小"), sidecarSizeSpin);

    auto *diagnosticFilesSpin = new QSpinBox(&dialog);
    diagnosticFilesSpin->setRange(1, 1000);
    diagnosticFilesSpin->setValue(cacheSettings.maxDiagnosticAudioFiles);
    formLayout->addRow(tr("诊断音频最多文件数"), diagnosticFilesSpin);

    auto *diagnosticAgeSpin = new QSpinBox(&dialog);
    diagnosticAgeSpin->setRange(1, 3650);
    diagnosticAgeSpin->setSuffix(tr(" 天"));
    diagnosticAgeSpin->setValue(cacheSettings.maxDiagnosticAudioAgeDays);
    formLayout->addRow(tr("诊断音频保留时间"), diagnosticAgeSpin);

    auto *diagnosticSizeSpin = new QSpinBox(&dialog);
    diagnosticSizeSpin->setRange(1, 102400);
    diagnosticSizeSpin->setSuffix(tr(" MiB"));
    diagnosticSizeSpin->setValue(cacheSettings.maxDiagnosticAudioMiB);
    formLayout->addRow(tr("诊断音频总大小"), diagnosticSizeSpin);

    auto *pcmCacheCombo = new QComboBox(&dialog);
    pcmCacheCombo->addItem(tr("禁用"), 0);
    pcmCacheCombo->addItem(tr("64 MB"), 64);
    pcmCacheCombo->addItem(tr("128 MB"), 128);
    pcmCacheCombo->addItem(tr("256 MB"), 256);
    pcmCacheCombo->addItem(tr("512 MB"), 512);
    pcmCacheCombo->addItem(tr("1 GB"), 1024);
    pcmCacheCombo->addItem(tr("2 GB"), 2048);
    const int currentPcmMiB = cacheSettings.maxPcmCacheMiB;
    int pcmComboIndex = 0;
    for (int i = 0; i < pcmCacheCombo->count(); ++i) {
        if (pcmCacheCombo->itemData(i).toInt() == currentPcmMiB) {
            pcmComboIndex = i;
            break;
        }
    }
    pcmCacheCombo->setCurrentIndex(pcmComboIndex);
    formLayout->addRow(tr("Seek 缓存内存上限"), pcmCacheCombo);

    layout->addLayout(formLayout);

    auto *usageLabel = new QLabel(&dialog);
    const auto refreshUsageLabel = [&] {
        usage = m_playbackSourceService.cacheUsage();
        usageLabel->setText(tr("当前占用：%1 / %2 个文件\nSidecar：%3 / %4 个\n诊断音频：%5 / %6 个\nLoopback 音频：%7 / %8 个")
                                .arg(formatDataSize(usage.totalBytes))
                                .arg(usage.totalFiles)
                                .arg(formatDataSize(usage.sidecarBytes))
                                .arg(usage.sidecarFiles)
                                .arg(formatDataSize(usage.diagnosticAudioBytes))
                                .arg(usage.diagnosticAudioFiles)
                                .arg(formatDataSize(usage.loopbackAudioBytes))
                                .arg(usage.loopbackAudioFiles));
    };
    refreshUsageLabel();
    layout->addWidget(usageLabel);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *pruneButton = buttonBox->addButton(tr("立即回收"), QDialogButtonBox::ActionRole);
    layout->addWidget(buttonBox);

    connect(browseButton, &QPushButton::clicked, this, [&] {
        const QString selectedDirectory =
            QFileDialog::getExistingDirectory(&dialog,
                                              tr("选择缓存目录"),
                                              directoryEdit->text().trimmed());
        if (!selectedDirectory.isEmpty()) {
            directoryEdit->setText(QDir::cleanPath(selectedDirectory));
        }
    });
    connect(pruneButton, &QPushButton::clicked, this, [&] {
        PlaybackCacheSettings pendingSettings = cacheSettings;
        pendingSettings.cacheDirectory = directoryEdit->text().trimmed();
        pendingSettings.maxSidecars = sidecarFilesSpin->value();
        pendingSettings.maxSidecarAgeDays = sidecarAgeSpin->value();
        pendingSettings.maxSidecarMiB = sidecarSizeSpin->value();
        pendingSettings.maxDiagnosticAudioFiles = diagnosticFilesSpin->value();
        pendingSettings.maxDiagnosticAudioAgeDays = diagnosticAgeSpin->value();
        pendingSettings.maxDiagnosticAudioMiB = diagnosticSizeSpin->value();
        pendingSettings.maxPcmCacheMiB = pcmCacheCombo->currentData().toInt();
        m_playbackSourceService.saveCacheSettings(pendingSettings);
        m_playbackSourceService.prunePlaybackCacheNow();
        cacheSettings = m_playbackSourceService.cacheSettings();
        refreshUsageLabel();
    });
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    cacheSettings.cacheDirectory = directoryEdit->text().trimmed();
    cacheSettings.maxSidecars = sidecarFilesSpin->value();
    cacheSettings.maxSidecarAgeDays = sidecarAgeSpin->value();
    cacheSettings.maxSidecarMiB = sidecarSizeSpin->value();
    cacheSettings.maxDiagnosticAudioFiles = diagnosticFilesSpin->value();
    cacheSettings.maxDiagnosticAudioAgeDays = diagnosticAgeSpin->value();
    cacheSettings.maxDiagnosticAudioMiB = diagnosticSizeSpin->value();
    cacheSettings.maxPcmCacheMiB = pcmCacheCombo->currentData().toInt();
    m_playbackSourceService.saveCacheSettings(cacheSettings);
    m_playbackSourceService.prunePlaybackCacheNow();
    statusBar()->showMessage(tr("缓存设置已保存"), 3000);
}
