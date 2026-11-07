// =============================================================================
// CrashHandler.cpp - implementation of the file logger and crash hooks
// declared in CrashHandler.h. See that header for the overview.
// =============================================================================
#include "CrashHandler.h"
#include <csignal>
#include <exception>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QCoreApplication>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Definition of the static mutex declared in the header.
QMutex CrashHandler::logMutex;

void CrashHandler::init() {
    // Start each run with a clean log instead of appending to whatever is left
    // over from previous launches.
    QFile logFile(resolveLogPath());
    static_cast<void>(logFile.open(QIODevice::WriteOnly | QIODevice::Truncate));

    // Route every qDebug/qWarning/qCritical/qFatal call into the log file.
    qInstallMessageHandler(CrashHandler::handleQtMessage);
    // Catch uncaught C++ exceptions.
    std::set_terminate(CrashHandler::handleTerminate);
    // Catch hard faults (access violation, FP error, illegal instruction, abort).
    std::signal(SIGSEGV, CrashHandler::handleSignal);
    std::signal(SIGFPE, CrashHandler::handleSignal);
    std::signal(SIGILL, CrashHandler::handleSignal);
    std::signal(SIGABRT, CrashHandler::handleSignal);
}

void CrashHandler::log(const QString &message) {
    // The mutex keeps concurrent callers from interleaving partial lines.
    QMutexLocker locker(&logMutex);
    logToFile(message);
}

QString CrashHandler::resolveLogPath() {
    // Before QCoreApplication exists applicationDirPath() is unusable, so fall
    // back to the current working directory.
    QString baseDir = QCoreApplication::instance()
                      ? QCoreApplication::applicationDirPath()
                      : QDir::currentPath();
    return QDir(baseDir).filePath("app.log");
}

void CrashHandler::logToFile(const QString &message) {
    // Open/append/close on every line so nothing is lost if we crash right after.
    QFile file(resolveLogPath());
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
            << " - " << message << "\n";
    }
}

void CrashHandler::handleQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &msg) {
    // Translate the severity enum to a readable prefix.
    QString level;
    switch (type) {
        case QtDebugMsg:    level = "QtDebug"; break;
        case QtInfoMsg:     level = "QtInfo"; break;
        case QtWarningMsg:  level = "QtWarning"; break;
        case QtCriticalMsg: level = "QtCritical"; break;
        case QtFatalMsg:    level = "QtFatal"; break;
    }

    // Append the source location when Qt provides it (debug / RelWithDebInfo).
    QString source;
    if (context.file || context.function || context.line > 0) {
        source = QString(" [file=%1 line=%2 func=%3]")
                    .arg(context.file ? context.file : "?")
                    .arg(context.line)
                    .arg(context.function ? context.function : "?");
    }

    QMutexLocker locker(&logMutex);
    logToFile(level + ": " + msg + source);

    // qFatal() is documented to terminate the program; honour that.
    if (type == QtFatalMsg) {
        std::abort();
    }
}

void CrashHandler::handleSignal(int signal) {
    // Human readable name for the most common crash signals.
    QString sigName;
    switch (signal) {
        case SIGSEGV: sigName = "SIGSEGV (Segmentation Fault)"; break;
        case SIGFPE:  sigName = "SIGFPE (Floating Point Exception)"; break;
        case SIGILL:  sigName = "SIGILL (Illegal Instruction)"; break;
        case SIGABRT: sigName = "SIGABRT (Abort)"; break;
        default:      sigName = QString("Signal %1").arg(signal); break;
    }

    log("CRASH: Caught signal " + sigName);

    // Try to flush logs and exit
    std::exit(signal);
}

void CrashHandler::handleTerminate() {
    log("CRASH: std::terminate() called. Uncaught exception?");

    // On Windows, we might be able to get the exception info if we were inside a try/catch block,
    // but std::terminate usually means we are past that.
    // We can try to rethrow to see what it is, but it's risky in a terminate handler.
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::exception& e) {
        log(QString("Uncaught exception: %1").arg(e.what()));
    } catch (...) {
        log("Uncaught unknown exception");
    }

    std::abort();
}
