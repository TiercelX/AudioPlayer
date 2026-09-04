#ifndef AUTOMATIONOPTIONS_H
#define AUTOMATIONOPTIONS_H

#include <optional>

class MainWindow;
class QCommandLineParser;

// If the ASIO probe CLI option was requested, run the probe and return an exit
// code. Returns std::nullopt when the probe option was not set and normal
// startup should continue.
std::optional<int> runAsioProbeIfRequested(const QCommandLineParser &parser);

// Register all automation CLI options on the given parser. Call this before
// parser.process().
void registerAutomationOptions(QCommandLineParser &parser);

// Read parsed automation CLI values and schedule the corresponding timed
// actions on the given MainWindow. Call this after parser.process() and after
// the MainWindow is shown.
void setupAutomationFromCli(const QCommandLineParser &parser, MainWindow &w);

#endif // AUTOMATIONOPTIONS_H
