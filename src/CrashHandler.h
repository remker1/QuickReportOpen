// =============================================================================
// CrashHandler.h - minimal diagnostic logger + crash hooks.
//
// Everything is written to "app.log" next to the executable. The class is
// purely static (no instances). It provides:
//   * CrashHandler::init()  - call once at program start.
//   * CrashHandler::log()   - thread-safe timestamped line logging.
//   * hooks that record Qt messages, fatal signals and uncaught C++ exceptions
//     so a crash on a customer machine leaves a trace in the log.
// =============================================================================
#ifndef CRASHHANDLER_H
#define CRASHHANDLER_H

#include <QString>
#include <QMutex>
#include <QtGlobal>

class CrashHandler {
public:
    // Truncates the log file and installs the Qt message handler, the
    // std::terminate handler and the C signal handlers.
    static void init();

    // Appends "<timestamp> - <message>" to the log. Safe to call from any thread.
    static void log(const QString &message);

private:
    // Raw append to the log file (caller must already hold logMutex).
    static void logToFile(const QString &message);
    // Absolute path of app.log (application dir, or CWD before QApplication exists).
    static QString resolveLogPath();
    // qInstallMessageHandler callback: mirrors qDebug/qWarning/... into the log.
    static void handleQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &msg);
    // std::signal callback for SIGSEGV/SIGFPE/SIGILL/SIGABRT.
    static void handleSignal(int signal);
    // std::set_terminate callback: logs the uncaught exception then aborts.
    static void handleTerminate();
    // Serialises writes so lines from different threads do not interleave.
    static QMutex logMutex;
};

#endif // CRASHHANDLER_H
