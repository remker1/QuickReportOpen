// =============================================================================
// FingerprintTestModule.cpp - UI logic of the fingerprint test page.
// =============================================================================
#include "FingerprintTestModule.h"
#include "common/Card.h"
#include "common/FingerprintScanner.h"
#include "common/TesterTheme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

FingerprintTestModule::FingerprintTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;}");
    auto *content = new QWidget(scroll);
    content->setStyleSheet("background:transparent;");
    auto *lay = new QVBoxLayout(content);
    lay->setContentsMargins(14, 10, 14, 10);
    lay->setSpacing(10);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    const auto &pal = TesterTheme::current();
    const QString noteStyle = QString("color:%1;font-size:11px;").arg(pal.textSecondary.name());

    // ── Intro ──
    m_infoCard = new Card(QString(), this);
    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_intro->setStyleSheet("font-size:14px;");
    m_infoCard->body()->addWidget(m_intro);
    lay->addWidget(m_infoCard);

    // ── Detection ──
    m_detectCard = new Card(QString(), this);
    m_detectLabel = new QLabel(this);
    m_detectLabel->setWordWrap(true);
    m_detectLabel->setStyleSheet("font-size:18px;font-weight:800;");
    m_detectCard->body()->addWidget(m_detectLabel);
    m_detectDetail = new QLabel(this);
    m_detectDetail->setWordWrap(true);
    m_detectDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detectDetail->setStyleSheet(QString("font-size:13px;color:%1;").arg(pal.textSecondary.name()));
    m_detectCard->body()->addWidget(m_detectDetail);
    lay->addWidget(m_detectCard);

    // ── Live scan ──
    m_scanCard = new Card(QString(), this);
    m_scanHint = new QLabel(this);
    m_scanHint->setWordWrap(true);
    m_scanHint->setStyleSheet("font-size:13px;");
    m_scanCard->body()->addWidget(m_scanHint);

    m_adminLabel = new QLabel(this);
    m_adminLabel->setWordWrap(true);
    m_adminLabel->setStyleSheet(noteStyle);
    m_scanCard->body()->addWidget(m_adminLabel);

    auto *ctrlRow = new QHBoxLayout();
    m_scanBtn = new QPushButton(this);
    m_cancelBtn = new QPushButton(this);
    m_cancelBtn->setVisible(false);
    ctrlRow->addWidget(m_scanBtn);
    ctrlRow->addWidget(m_cancelBtn);
    ctrlRow->addStretch();
    m_scanCard->body()->addLayout(ctrlRow);

    m_scanStatus = new QLabel(this);
    m_scanStatus->setWordWrap(true);
    m_scanStatus->setStyleSheet("font-size:13px;font-weight:600;margin-top:4px;");
    m_scanCard->body()->addWidget(m_scanStatus);

    lay->addWidget(m_scanCard);
    lay->addStretch();

    m_scanner = new FingerprintScanner(this);
    connect(m_scanner, &FingerprintScanner::captured, this,
            &FingerprintTestModule::onCaptured);
    connect(m_scanner, &FingerprintScanner::failed, this,
            &FingerprintTestModule::onFailed);
    connect(m_scanBtn, &QPushButton::clicked, this, &FingerprintTestModule::startScan);
    connect(m_cancelBtn, &QPushButton::clicked, this, [this] { m_scanner->cancel(); });

    retranslate();
}

// Cancels a running scan on destruction.
FingerprintTestModule::~FingerprintTestModule() {
    if (m_scanner)
        m_scanner->cancel(); // scanner dtor joins the worker
}

// Entering the page re-runs detection.
void FingerprintTestModule::moduleEntered() { detect(); }

// Leaving the page cancels any capture in progress.
void FingerprintTestModule::moduleLeft() {
    if (m_scanner->isRunning())
        m_scanner->cancel();
}

// Enumerates sensors, fills the detection card and records the result for the report.
void FingerprintTestModule::detect() {
    const auto &pal = TesterTheme::current();

    const bool admin = FingerprintScanner::processElevated();
    m_adminLabel->setText(
        tr_(admin ? "ht_fp_admin_yes" : "ht_fp_admin_no"));
    m_adminLabel->setStyleSheet(QString("font-size:11px;color:%1;")
                                    .arg((admin ? pal.textSecondary : pal.warn).name()));

    if (!FingerprintScanner::platformSupported()) {
        m_present = false;
        m_detectLabel->setStyleSheet("font-size:18px;font-weight:800;");
        m_detectLabel->setText(tr_("ht_common_not_supported"));
        m_detectDetail->clear();
        m_scanBtn->setEnabled(false);
        recordResult("ht_fp_result_name", QStringLiteral("Not supported on this platform"));
        return;
    }

    const QList<FingerprintUnit> units = FingerprintScanner::enumerateUnits();
    m_present = !units.isEmpty();

    if (m_present) {
        m_detectLabel->setStyleSheet(QString("font-size:18px;font-weight:800;color:%1;")
                                         .arg(pal.ok.name()));
        m_detectLabel->setText(tr_("ht_fp_present"));
        QStringList lines;
        for (const FingerprintUnit &u : units) {
            QString name = u.description;
            QString maker = u.manufacturer;
            if (!u.model.isEmpty() && u.model != u.description)
                name += " · " + u.model;
            if (!maker.isEmpty())
                name = maker + " — " + name;
            if (name.trimmed().isEmpty())
                name = tr_("ht_fp_unit_generic").replace("{id}", QString::number(u.unitId));
            lines << name;
        }
        m_detectDetail->setText(lines.join("\n"));
        m_scanBtn->setEnabled(true);
        recordResult("ht_fp_result_name",
                     QStringLiteral("DETECTED - sensor present, driver responding (%1)")
                         .arg(lines.join("; ")));
    } else {
        m_detectLabel->setStyleSheet(QString("font-size:18px;font-weight:800;color:%1;")
                                         .arg(pal.warn.name()));
        m_detectLabel->setText(tr_("ht_fp_absent"));
        m_detectDetail->setText(tr_("ht_fp_absent_detail"));
        m_scanBtn->setEnabled(false);
        recordResult("ht_fp_result_name",
                     QStringLiteral("NOT DETECTED - no fingerprint sensor / driver"));
    }
}

// Starts a one-shot capture and switches the UI to the scanning state.
void FingerprintTestModule::startScan() {
    setScanning(true);
    m_scanStatus->setStyleSheet(QString("font-size:13px;font-weight:600;color:%1;")
                                    .arg(TesterTheme::current().accent.name()));
    m_scanStatus->setText(tr_("ht_fp_scanning"));
    m_scanner->startCapture();
}

// Capture succeeded: show it and record a PASS-style result.
void FingerprintTestModule::onCaptured(quint32 unitId, qint64 bytes) {
    Q_UNUSED(unitId);
    Q_UNUSED(bytes);
    setScanning(false);
    const auto &pal = TesterTheme::current();
    m_scanStatus->setStyleSheet(QString("font-size:14px;font-weight:800;color:%1;")
                                    .arg(pal.ok.name()));
    m_scanStatus->setText(tr_("ht_fp_captured"));
    recordResult("ht_fp_result_name",
                 QStringLiteral("PASS - sensor responded to a finger (live scan)"));
}

// Capture failed: show a message depending on the kind (access/busy/cancelled/...).
void FingerprintTestModule::onFailed(const QString &kind, long code) {
    setScanning(false);
    const auto &pal = TesterTheme::current();
    QString key;
    if (kind == "access")
        // Elevated already? Then it's the adapter refusing capture, not a
        // missing-admin problem — say so instead of pointing at admin again.
        key = FingerprintScanner::processElevated() ? "ht_fp_err_refused"
                                                     : "ht_fp_err_access";
    else if (kind == "notimpl")   key = "ht_fp_err_notimpl";
    else if (kind == "busy")      key = "ht_fp_err_busy";
    else if (kind == "cancelled") key = "ht_fp_err_cancelled";
    else if (kind == "nosensor")  key = "ht_fp_err_nosensor";
    else                          key = "ht_fp_err_other";

    QString msg = tr_(key);
    if (key == "ht_fp_err_other")
        msg = msg.replace("{code}", QString("0x%1").arg(static_cast<quint32>(code), 8, 16, QChar('0')));

    const bool soft = (kind == "cancelled"); // cancel isn't a hardware fault
    m_scanStatus->setStyleSheet(QString("font-size:13px;font-weight:700;color:%1;")
                                    .arg((soft ? pal.textSecondary : pal.error).name()));
    m_scanStatus->setText(msg);
}

// Toggles Scan/Cancel button state.
void FingerprintTestModule::setScanning(bool on) {
    m_scanBtn->setVisible(!on);
    m_cancelBtn->setVisible(on);
}

// Re-applies translated texts.
void FingerprintTestModule::retranslate() {
    m_infoCard->setTitle(tr_("ht_fp_intro_title"));
    m_detectCard->setTitle(tr_("ht_fp_detect_title"));
    m_scanCard->setTitle(tr_("ht_fp_scan_title"));
    m_intro->setText(tr_("ht_fp_intro"));
    m_scanHint->setText(tr_("ht_fp_scan_hint"));
    m_scanBtn->setText(tr_("ht_fp_start_btn"));
    m_cancelBtn->setText(tr_("ht_common_cancel_test"));
}
