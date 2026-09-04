#ifndef PLAYERLOGGER_H
#define PLAYERLOGGER_H

#include <QVariantMap>
#include <QString>

namespace PlayerLogger {

QString logFilePath();
QString diagnosticLogFilePath();
bool highVolumeJsonlDiagnosticsEnabled();
void log(const QString &category, const QString &message);
void diagnostic(const QString &category,
                const QString &eventName,
                const QVariantMap &fields = {});

} // namespace PlayerLogger

#endif // PLAYERLOGGER_H
