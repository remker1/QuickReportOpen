// =============================================================================
// FingerprintScanner.cpp - WinBio enumeration and single-sample capture.
// =============================================================================
#include "FingerprintScanner.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winbio.h>

#include <QMetaObject>

#pragma comment(lib, "Winbio.lib")

namespace {

// Turn a WinBio/HRESULT failure into a stable kind the UI can translate.
QString classify(HRESULT hr) {
    switch (hr) {
    case E_ACCESSDENIED:
    case WINBIO_E_ENROLLMENT_IN_PROGRESS:
        return QStringLiteral("access");
    case E_NOTIMPL:
    case WINBIO_E_UNSUPPORTED_FACTOR:
        // Secure / match-on-chip sensors (e.g. Enhanced Sign-in Security) do
        // not let software read the sensor directly.
        return QStringLiteral("notimpl");
    case WINBIO_E_CANCELED:
        return QStringLiteral("cancelled");
    case WINBIO_E_DEVICE_BUSY:
    case WINBIO_E_SESSION_BUSY:
    case WINBIO_E_LOCK_VIOLATION:
        return QStringLiteral("busy");
    case WINBIO_E_UNKNOWN_ID:
    case WINBIO_E_SENSOR_UNAVAILABLE:
    case WINBIO_E_DEVICE_FAILURE:
        return QStringLiteral("nosensor");
    default:
        return QStringLiteral("other");
    }
}

// Wide string -> QString.
QString fromWide(const wchar_t *s) {
    return s ? QString::fromWCharArray(s) : QString();
}

} // namespace

// (trivial) QObject constructor.
FingerprintScanner::FingerprintScanner(QObject *parent) : QObject(parent) {}

// Cancels any running capture and joins the worker thread.
FingerprintScanner::~FingerprintScanner() {
    cancel();
    joinWorker();
}

// (see header)
bool FingerprintScanner::platformSupported() { return true; }

// (see header) Checks the process token elevation.
bool FingerprintScanner::processElevated() {
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation,
                                sizeof(elevation), &size))
            elevated = elevation.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

// (see header) WinBioEnumBiometricUnits for the fingerprint factor.
QList<FingerprintUnit> FingerprintScanner::enumerateUnits() {
    QList<FingerprintUnit> out;
    WINBIO_UNIT_SCHEMA *schema = nullptr;
    SIZE_T count = 0;
    const HRESULT hr =
        WinBioEnumBiometricUnits(WINBIO_TYPE_FINGERPRINT, &schema, &count);
    if (SUCCEEDED(hr) && schema) {
        for (SIZE_T i = 0; i < count; ++i) {
            FingerprintUnit u;
            u.unitId = static_cast<quint32>(schema[i].UnitId);
            u.description = fromWide(schema[i].Description);
            u.manufacturer = fromWide(schema[i].Manufacturer);
            u.model = fromWide(schema[i].Model);
            out.append(u);
        }
    }
    if (schema)
        WinBioFree(schema);
    return out;
}

// (see header) Worker thread: opens a system-pool fingerprint session, calls
// WinBioLocateSensor (waits for a finger touch; extracts no biometric data, so it
// also works on match-on-chip sensors) and reports captured()/failed() back on the
// GUI thread.
void FingerprintScanner::startCapture() {
    if (m_running.exchange(true))
        return; // already capturing
    joinWorker(); // reap a previous finished worker before launching a new one

    m_worker = std::thread([this] {
        auto emitFailed = [this](const QString &kind, HRESULT hr) {
            const long code = static_cast<long>(hr);
            QMetaObject::invokeMethod(
                this, [this, kind, code] { emit failed(kind, code); },
                Qt::QueuedConnection);
        };

        WINBIO_SESSION_HANDLE session = 0;
        HRESULT hr = WinBioOpenSession(
            WINBIO_TYPE_FINGERPRINT, WINBIO_POOL_SYSTEM, WINBIO_FLAG_DEFAULT,
            nullptr, 0, nullptr, &session);
        if (FAILED(hr)) {
            m_running.store(false);
            emitFailed(classify(hr), hr);
            return;
        }
        m_session.store(static_cast<quintptr>(session));

        // WinBioLocateSensor asks the sensor to wait for a finger touch and
        // returns which unit detected it. It extracts no biometric data, so it
        // works on secure / match-on-chip sensors that refuse WinBioCaptureSample
        // (E_NOTIMPL) and never sets up or reads a fingerprint. A returned unit
        // is proof the sensor physically responded to the finger.
        WINBIO_UNIT_ID unitId = 0;
        hr = WinBioLocateSensor(session, &unitId);

        // Session is done; clear the shared handle before closing so a late
        // cancel() can't act on a freed handle.
        m_session.store(0);
        const quint32 uid = static_cast<quint32>(unitId);
        WinBioCloseSession(session);
        m_running.store(false);

        if (SUCCEEDED(hr)) {
            QMetaObject::invokeMethod(
                this, [this, uid] { emit captured(uid, 0); },
                Qt::QueuedConnection);
        } else {
            emitFailed(classify(hr), hr);
        }
    });
}

// (see header) Uses WinBioCancel on the live session handle.
void FingerprintScanner::cancel() {
    const quintptr s = m_session.load();
    if (s)
        WinBioCancel(static_cast<WINBIO_SESSION_HANDLE>(s));
}

// Waits for the worker thread to end.
void FingerprintScanner::joinWorker() {
    if (m_worker.joinable())
        m_worker.join();
}
