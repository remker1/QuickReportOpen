// =============================================================================
// CloneProgressDialog.cpp - implementation of the clone progress dialog.
// =============================================================================
#include "CloneProgressDialog.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

// Formats bytes as KB / MB / GB (GB with one decimal) for the "Copied" tile.
static QString fmtBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024 * 1024), 'f', 1) + " GB";
    if (bytes >= 1024LL * 1024)
        return QString::number(bytes / (1024.0 * 1024), 'f', 0) + " MB";
    return QString::number(bytes / 1024.0, 'f', 0) + " KB";
}

// Formats seconds as m:ss, or h:mm:ss when an hour or more.
static QString fmtDuration(qint64 secs)
{
    if (secs < 0)
        secs = 0;
    const qint64 h = secs / 3600;
    const qint64 m = (secs % 3600) / 60;
    const qint64 s = secs % 60;
    if (h > 0)
        return QString("%1:%2:%3").arg(h)
            .arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QString("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// Builds the dialog: style sheet, source -> target disk cards, big percent,
// stage line, progress bar, 4 stat tiles, shutdown checkbox and Cancel button.
CloneProgressDialog::CloneProgressDialog(const Translator &tr, QWidget *parent)
    : QDialog(parent), tr_(tr)
{
    setWindowModality(Qt::WindowModal);
    setMinimumWidth(480);
    setStyleSheet(
        "QDialog { background: #FDFEFF; }"
        "QLabel { color: #33475B; background: transparent; }"
        "QFrame#DiskCard { background: #F2F7FB; border: 1px solid #D6DEE6; border-radius: 10px; }"
        "QLabel#DiskTitle { font-weight: bold; color: #2C5F8A; font-size: 13px; }"
        "QLabel#DiskDetail { color: #6a7a8a; font-size: 11px; }"
        "QLabel#Arrow { color: #4A90D9; font-size: 22px; }"
        "QLabel#Percent { color: #2C5F8A; font-size: 34px; font-weight: bold; }"
        "QLabel#Stage { color: #6a7a8a; font-size: 12px; }"
        "QLabel#StatValue { color: #33475B; font-size: 13px; font-weight: bold; }"
        "QLabel#StatCaption { color: #8a98a8; font-size: 11px; }"
        "QProgressBar { background: #E4EBF2; border: none; border-radius: 7px; }"
        "QProgressBar::chunk { border-radius: 7px;"
        "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #5AA2E0, stop:1 #2C5F8A); }"
        "QCheckBox { color: #5C7C98; background: transparent; }"
        "QPushButton { background: #FFFFFF; border: 1px solid #C9D4DE; border-radius: 6px;"
        "  padding: 6px 18px; color: #33475B; }"
        "QPushButton:hover { background: #F2F7FB; }"
        "QPushButton:disabled { color: #9AA7B4; }");

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 20, 24, 18);
    lay->setSpacing(13);

    // Source ➜ target disk cards.
    auto *diskRow = new QHBoxLayout;
    diskRow->setSpacing(10);
    diskRow->addWidget(makeDiskCard(&m_srcTitle, &m_srcDetail), 1);
    auto *arrow = new QLabel(QString::fromUtf8("➜"), this);
    arrow->setObjectName("Arrow");
    arrow->setAlignment(Qt::AlignCenter);
    diskRow->addWidget(arrow, 0);
    diskRow->addWidget(makeDiskCard(&m_tgtTitle, &m_tgtDetail), 1);
    lay->addLayout(diskRow);

    m_percentLabel = new QLabel("0%", this);
    m_percentLabel->setObjectName("Percent");
    m_percentLabel->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_percentLabel);

    m_stageLabel = new QLabel(this);
    m_stageLabel->setObjectName("Stage");
    m_stageLabel->setAlignment(Qt::AlignCenter);
    m_stageLabel->setWordWrap(true);
    lay->addWidget(m_stageLabel);

    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFixedHeight(14);
    lay->addWidget(m_bar);

    auto *stats = new QGridLayout;
    stats->setHorizontalSpacing(8);
    stats->setVerticalSpacing(2);
    auto addStat = [&](int col, const QString &captionKey, QLabel **value) {
        *value = new QLabel(QString::fromUtf8("—"), this);
        (*value)->setObjectName("StatValue");
        (*value)->setAlignment(Qt::AlignCenter);
        auto *caption = new QLabel(tr_(captionKey), this);
        caption->setObjectName("StatCaption");
        caption->setAlignment(Qt::AlignCenter);
        stats->addWidget(*value, 0, col);
        stats->addWidget(caption, 1, col);
        stats->setColumnStretch(col, 1);
    };
    addStat(0, "clone_stat_copied", &m_copiedValue);
    addStat(1, "clone_stat_speed", &m_speedValue);
    addStat(2, "clone_stat_eta", &m_etaValue);
    addStat(3, "clone_stat_elapsed", &m_elapsedValue);
    lay->addLayout(stats);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setStyleSheet("background: #E4EBF2; border: none; max-height: 1px;");
    lay->addWidget(line);

    auto *bottom = new QHBoxLayout;
    m_shutdownCheck = new QCheckBox(tr_("clone_shutdown_after_success"), this);
    bottom->addWidget(m_shutdownCheck, 1);
    m_cancelBtn = new QPushButton(tr_("cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this, &CloneProgressDialog::requestCancel);
    bottom->addWidget(m_cancelBtn, 0);
    lay->addLayout(bottom);

    m_elapsed.start();
}

// (see header)
QWidget *CloneProgressDialog::makeDiskCard(QLabel **title, QLabel **detail)
{
    auto *card = new QFrame(this);
    card->setObjectName("DiskCard");
    auto *v = new QVBoxLayout(card);
    v->setContentsMargins(12, 9, 12, 9);
    v->setSpacing(2);
    *title = new QLabel(card);
    (*title)->setObjectName("DiskTitle");
    *detail = new QLabel(card);
    (*detail)->setObjectName("DiskDetail");
    (*detail)->setWordWrap(true);
    v->addWidget(*title);
    v->addWidget(*detail);
    return card;
}

void CloneProgressDialog::setDisks(const QString &sourceTitle, const QString &sourceDetail,
                                   const QString &targetTitle, const QString &targetDetail)
{
    m_srcTitle->setText(sourceTitle);
    m_srcDetail->setText(sourceDetail);
    m_tgtTitle->setText(targetTitle);
    m_tgtDetail->setText(targetDetail);
}

void CloneProgressDialog::setStage(const QString &text)
{
    // Once cancelling, keep the cancel notice on screen until the worker ends.
    if (!m_cancelRequested)
        m_stageLabel->setText(text);
}

bool CloneProgressDialog::shutdownRequested() const
{
    return m_shutdownCheck->isChecked();
}

void CloneProgressDialog::setValue(int percent)
{
    percent = qBound(0, percent, 100);
    m_bar->setValue(percent);
    m_percentLabel->setText(QString::number(percent) + "%");
}

void CloneProgressDialog::setDetail(int percent, qint64 copiedBytes, qint64 totalBytes,
                                    double mibPerSec, qint64 etaSecs)
{
    setValue(percent);
    m_copiedValue->setText(fmtBytes(copiedBytes) + " / " + fmtBytes(qMax<qint64>(totalBytes, 1)));
    m_speedValue->setText(QString::number(mibPerSec, 'f', 1) + " MiB/s");
    m_etaValue->setText(mibPerSec > 0.0 ? fmtDuration(etaSecs) : QString::fromUtf8("—"));
    m_elapsedValue->setText(fmtDuration(m_elapsed.elapsed() / 1000));
}

// (see header) Idempotent: only the first call has an effect.
void CloneProgressDialog::requestCancel()
{
    if (m_cancelRequested)
        return;
    m_cancelRequested = true;
    m_stageLabel->setText(tr_("clone_cancelling"));
    m_cancelBtn->setEnabled(false);
    emit canceled();
}

// Window [X] cancels instead of closing, unless signals are blocked (the finish
// handler blocks them to close the dialog programmatically).
void CloneProgressDialog::closeEvent(QCloseEvent *event)
{
    if (signalsBlocked()) {
        QDialog::closeEvent(event);
        return;
    }
    event->ignore();
    requestCancel();
}

// Esc is routed to the cancel flow rather than closing the dialog.
void CloneProgressDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        requestCancel();
        return;
    }
    QDialog::keyPressEvent(event);
}
