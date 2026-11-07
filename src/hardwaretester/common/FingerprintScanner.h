// =============================================================================
// FingerprintScanner.h - Windows Biometric Framework probe (see class comment).
// =============================================================================
#ifndef FINGERPRINTSCANNER_H
#define FINGERPRINTSCANNER_H

#include <QObject>
#include <QString>
#include <QList>

#include <atomic>
#include <thread>

// Fingerprint-reader probe built on the Windows Biometric Framework (WBF).
//
// Two independent checks, neither of which enrolls or stores anything:
//   1. enumerateUnits() — synchronous, no interaction, no elevation. A unit
//      coming back means the sensor is present and its driver stack is healthy
//      (the same thing that lets Windows Hello offer fingerprint sign-in).
//   2. startCapture() — asks the user to tap the sensor once and captures a
//      single sample. A captured sample is proof the reader physically works.
//
// All Windows/WinBio types are confined to the .cpp so this header stays clean.
struct FingerprintUnit {
    quint32 unitId = 0;
    QString description;   // e.g. "Synaptics WBDI"
    QString manufacturer;
    QString model;
};

class FingerprintScanner : public QObject {
    Q_OBJECT
public:
    explicit FingerprintScanner(QObject *parent = nullptr);
    ~FingerprintScanner() override;

    // True on any platform where WinBio is available (all supported Windows).
    static bool platformSupported();

    // Whether this process runs with an elevated (administrator) token. Raw
    // system-pool capture requires it; shown on the page so a capture failure
    // can be told apart from a missing-elevation problem.
    static bool processElevated();

    // Cheap, blocking, no user interaction. Empty list = no fingerprint sensor.
    static QList<FingerprintUnit> enumerateUnits();

    // Opens a system-pool session on a worker thread and waits for one finger.
    // Emits captured() or failed() exactly once. No-op if already running.
    void startCapture();
    // Aborts an in-flight capture; the worker then emits failed(kind="cancelled").
    void cancel();
    bool isRunning() const { return m_running.load(); }

signals:
    void captured(quint32 unitId, qint64 sampleBytes);
    // kind: "access" | "busy" | "cancelled" | "nosensor" | "unsupported" | "other"
    void failed(const QString &kind, long code);

private:
    void joinWorker();

    std::thread m_worker;
    std::atomic<bool> m_running{false};
    // Live WINBIO_SESSION_HANDLE (ULONG_PTR) so cancel() can reach the blocking
    // capture from the GUI thread; 0 when no session is open.
    std::atomic<quintptr> m_session{0};
};

#endif // FINGERPRINTSCANNER_H
