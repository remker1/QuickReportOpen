// =============================================================================
// ScreenTestModule.cpp - monitor cards and the fill-colour test dialog.
// =============================================================================
#include "ScreenTestModule.h"
#include "common/Card.h"
#include "common/TesterTheme.h"

#include <QGridLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

// ── FillPatternDialog ──────────────────────────────────────────

FillPatternDialog::FillPatternDialog(QScreen *screen, const QString &hint, QWidget *parent)
    : QDialog(parent, Qt::FramelessWindowHint | Qt::Window), m_hint(hint) {
    setModal(true);
    if (screen) {
        setScreen(screen);
        setGeometry(screen->geometry());
    }
    setCursor(Qt::BlankCursor);
}

// Next/previous pattern; accepts (closes) when past the last one.
void FillPatternDialog::advance(int step) {
    constexpr int kStages = 7;
    m_stage += step;
    if (m_stage < 0)
        m_stage = kStages - 1;
    if (m_stage >= kStages) {
        accept();
        return;
    }
    update();
}

// Paints the current solid colour or gradient over the whole screen plus the hint.
void FillPatternDialog::paintEvent(QPaintEvent *) {
    QPainter p(this);
    switch (m_stage) {
        case 0: p.fillRect(rect(), Qt::white); break;
        case 1: p.fillRect(rect(), Qt::black); break;
        case 2: p.fillRect(rect(), QColor(255, 0, 0)); break;
        case 3: p.fillRect(rect(), QColor(0, 255, 0)); break;
        case 4: p.fillRect(rect(), QColor(0, 0, 255)); break;
        case 5: p.fillRect(rect(), QColor(128, 128, 128)); break;
        case 6: {
            QLinearGradient g(0, 0, width(), 0);
            g.setColorAt(0.0, Qt::black);
            g.setColorAt(1.0, Qt::white);
            p.fillRect(rect(), g);
            break;
        }
    }
    // Subtle hint in the corner; readable on any stage color.
    const QColor fg = (m_stage == 1) ? QColor(140, 140, 140) : QColor(90, 90, 90);
    p.setPen(fg);
    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    p.drawText(rect().adjusted(12, 8, -12, -8), Qt::AlignBottom | Qt::AlignLeft, m_hint);
}

// Click = next pattern.
void FillPatternDialog::mousePressEvent(QMouseEvent *) { advance(1); }

// Arrow keys step through the patterns; Esc closes.
void FillPatternDialog::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
        case Qt::Key_Escape: reject(); break;
        case Qt::Key_Left:
        case Qt::Key_Up: advance(-1); break;
        default: advance(1); break;
    }
}

// ── ScreenTestModule ───────────────────────────────────────────

ScreenTestModule::ScreenTestModule(Translator t, QWidget *parent)
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

    m_usageLabel = new QLabel(content);
    m_usageLabel->setWordWrap(true);
    m_usageLabel->setStyleSheet(QString("color:%1;font-size:12px;")
                                    .arg(TesterTheme::current().textSecondary.name()));
    lay->addWidget(m_usageLabel);

    m_cardsLayout = new QVBoxLayout();
    m_cardsLayout->setSpacing(10);
    lay->addLayout(m_cardsLayout);
    lay->addStretch();

    scroll->setWidget(content);
    outer->addWidget(scroll);

    retranslate();
}

// Refreshes the monitor cards each time the page is shown.
void ScreenTestModule::moduleEntered() {
    rebuildCards();
}

// Queries DisplayInfoProvider and builds a card per monitor (name, resolution,
// refresh, HDR, colour depth, EDID) with a "Fill test" button.
void ScreenTestModule::rebuildCards() {
    qDeleteAll(m_cards);
    m_cards.clear();

    m_monitors = DisplayInfoProvider::queryMonitors();

    QStringList summaryParts;
    for (const MonitorInfo &mi : m_monitors) {
        QString title = mi.friendlyName.isEmpty() ? mi.gdiDeviceName : mi.friendlyName;
        if (title.isEmpty())
            title = tr_("ht_scr_unknown_monitor");
        auto *card = new Card(title, this);

        auto *grid = new QGridLayout();
        grid->setHorizontalSpacing(24);
        grid->setVerticalSpacing(4);
        int row = 0;
        auto addRow = [&](const QString &key, const QString &value) {
            auto *k = new QLabel(tr_(key), card);
            k->setStyleSheet(QString("color:%1;").arg(
                TesterTheme::current().textSecondary.name()));
            auto *v = new QLabel(value, card);
            v->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(k, row, 0);
            grid->addWidget(v, row, 1);
            ++row;
        };

        addRow("ht_scr_resolution", QString("%1 × %2").arg(mi.pixelWidth).arg(mi.pixelHeight));
        addRow("ht_scr_refresh",
               mi.refreshHz > 0 ? QString::number(mi.refreshHz, 'f', 3) + " Hz"
                                : tr_("ht_common_not_supported"));

        // Scale + color depth via the matching QScreen (physical px / logical px).
        for (QScreen *s : QGuiApplication::screens()) {
            if (s->name() == mi.gdiDeviceName) {
                addRow("ht_scr_scale",
                       QString::number(qRound(s->devicePixelRatio() * 100)) + " %");
                addRow("ht_scr_depth", QString::number(s->depth()) + " bit");
                break;
            }
        }

        QString hdr = mi.hdrSupported
            ? tr_(mi.hdrEnabled ? "ht_scr_hdr_on" : "ht_scr_hdr_off")
            : tr_("ht_scr_hdr_unsupported");
        addRow("ht_scr_hdr", hdr);
        if (mi.bitsPerChannel > 0)
            addRow("ht_scr_bits_per_channel",
                   QString("%1 bit%2").arg(mi.bitsPerChannel)
                       .arg(mi.colorEncoding.isEmpty() ? QString()
                                                       : " · " + mi.colorEncoding));
        addRow("ht_scr_edid",
               mi.edidModel.isEmpty() && mi.edidVendor.isEmpty()
                   ? tr_("ht_scr_edid_unreadable")
                   : QString("%1 %2").arg(mi.edidVendor, mi.edidModel).trimmed());

        card->body()->addLayout(grid);

        auto *fillBtn = new QPushButton(tr_("ht_scr_fill_test"), card);
        const MonitorInfo miCopy = mi;
        connect(fillBtn, &QPushButton::clicked, this,
                [this, miCopy] { launchFillTest(miCopy); });
        auto *btnRow = new QHBoxLayout();
        btnRow->addWidget(fillBtn);
        btnRow->addStretch();
        card->body()->addLayout(btnRow);

        m_cardsLayout->addWidget(card);
        m_cards.append(card);

        summaryParts << QString("%1x%2@%3")
                            .arg(mi.pixelWidth).arg(mi.pixelHeight)
                            .arg(mi.refreshHz > 0 ? QString::number(mi.refreshHz, 'f', 3)
                                                  : "?");
    }

    if (m_monitors.isEmpty()) {
        auto *card = new Card(tr_("ht_common_not_supported"), this);
        m_cardsLayout->addWidget(card);
        m_cards.append(card);
    } else {
        recordResult("ht_scr_result_name",
                     QString("%1 display(s): %2")
                         .arg(m_monitors.size())
                         .arg(summaryParts.join("; ")));
    }
}

// Shows FillPatternDialog full-screen on the QScreen matching the monitor.
void ScreenTestModule::launchFillTest(const MonitorInfo &info) {
    QScreen *target = nullptr;
    for (QScreen *s : QGuiApplication::screens())
        if (s->name() == info.gdiDeviceName)
            target = s;
    if (!target)
        target = screen();

    FillPatternDialog dlg(target, tr_("ht_scr_fill_hint"), this);
    dlg.showFullScreen();
    dlg.exec();
}

// Re-applies translated texts and rebuilds the cards.
void ScreenTestModule::retranslate() {
    m_usageLabel->setText(tr_("ht_scr_usage"));
    if (isVisible())
        rebuildCards();
}
