// =============================================================================
// FingerprintTestModule.h - fingerprint-reader presence and capture test.
// =============================================================================
#ifndef FINGERPRINTTESTMODULE_H
#define FINGERPRINTTESTMODULE_H

#include "TesterModule.h"

class QLabel;
class QPushButton;
class Card;
class FingerprintScanner;

// Fingerprint-reader test. Detection (WinBio unit enumeration) confirms the
// sensor + driver are present without any interaction; the optional live scan
// captures one sample to prove the reader physically works. Nothing is enrolled
// or stored — this never sets up fingerprint sign-in.
class FingerprintTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit FingerprintTestModule(Translator t, QWidget *parent = nullptr);
    ~FingerprintTestModule() override;

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    void detect();                 // enumerate units, update detection card
    void startScan();
    void onCaptured(quint32 unitId, qint64 bytes);
    void onFailed(const QString &kind, long code);
    void setScanning(bool on);

    // UI
    Card *m_infoCard = nullptr;
    Card *m_detectCard = nullptr;
    Card *m_scanCard = nullptr;
    QLabel *m_intro = nullptr;
    QLabel *m_detectLabel = nullptr;    // headline present/absent line
    QLabel *m_detectDetail = nullptr;   // sensor name / manufacturer
    QLabel *m_scanHint = nullptr;
    QLabel *m_adminLabel = nullptr;   // "running as administrator: yes/no"
    QPushButton *m_scanBtn = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QLabel *m_scanStatus = nullptr;

    FingerprintScanner *m_scanner = nullptr;
    bool m_present = false;
};

#endif // FINGERPRINTTESTMODULE_H
