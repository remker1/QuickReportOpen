// =============================================================================
// PowerTestModule.cpp - live charger/battery readings page.
// =============================================================================
#include "PowerTestModule.h"
#include "common/Card.h"
#include "common/PowerMetrics.h"
#include "common/TesterTheme.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {
// One big value + small caption stacked in a column of the live-metrics grid.
// A big number with a caption underneath (voltage / current / power tiles).
struct Readout {
    QLabel *value;
    QLabel *caption;
};
// Creates one Readout tile styled with the palette.
Readout makeReadout(QWidget *parent, const TesterTheme::Palette &pal) {
    Readout r;
    r.value = new QLabel(parent);
    r.value->setAlignment(Qt::AlignCenter);
    r.value->setStyleSheet(QString("font-size:30px;font-weight:800;color:%1;")
                               .arg(pal.textPrimary.name()));
    r.caption = new QLabel(parent);
    r.caption->setAlignment(Qt::AlignCenter);
    r.caption->setStyleSheet(QString("font-size:12px;color:%1;").arg(pal.textSecondary.name()));
    return r;
}
} // namespace

PowerTestModule::PowerTestModule(Translator t, QWidget *parent)
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

    // ── Power source ──
    m_sourceCard = new Card(QString(), this);
    m_sourceLabel = new QLabel(this);
    m_sourceLabel->setStyleSheet("font-size:22px;font-weight:800;");
    m_sourceCard->body()->addWidget(m_sourceLabel);
    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    m_stateLabel->setStyleSheet(QString("font-size:13px;color:%1;").arg(pal.textSecondary.name()));
    m_sourceCard->body()->addWidget(m_stateLabel);
    lay->addWidget(m_sourceCard);

    // ── Live measurements ──
    m_liveCard = new Card(QString(), this);

    // Headline: whole-system live power draw (the number the technician cares
    // about). Measurable only on battery, where discharge = total consumption.
    m_drawVal = new QLabel(this);
    m_drawVal->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_drawVal->setStyleSheet(QString("font-size:40px;font-weight:900;color:%1;")
                                 .arg(pal.textPrimary.name()));
    m_drawCap = new QLabel(this);
    m_drawCap->setStyleSheet(QString("font-size:12px;color:%1;").arg(pal.textSecondary.name()));
    m_drawExplain = new QLabel(this);
    m_drawExplain->setWordWrap(true);
    m_drawExplain->setStyleSheet(QString("font-size:12px;color:%1;").arg(pal.textSecondary.name()));
    m_liveCard->body()->addWidget(m_drawCap);
    m_liveCard->body()->addWidget(m_drawVal);
    m_liveCard->body()->addWidget(m_drawExplain);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QString("color:%1;background:%1;max-height:1px;margin:8px 0;")
                           .arg(pal.cardBorder.name()));
    m_liveCard->body()->addWidget(sep);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(2);
    Readout v = makeReadout(this, pal);
    Readout a = makeReadout(this, pal);
    Readout w = makeReadout(this, pal);
    m_voltageVal = v.value; m_voltageCap = v.caption;
    m_currentVal = a.value; m_currentCap = a.caption;
    m_powerVal = w.value;   m_powerCap = w.caption;
    grid->addWidget(m_voltageVal, 0, 0);
    grid->addWidget(m_currentVal, 0, 1);
    grid->addWidget(m_powerVal, 0, 2);
    grid->addWidget(m_voltageCap, 1, 0);
    grid->addWidget(m_currentCap, 1, 1);
    grid->addWidget(m_powerCap, 1, 2);
    m_liveCard->body()->addLayout(grid);

    m_flowLabel = new QLabel(this);
    m_flowLabel->setWordWrap(true);
    m_flowLabel->setStyleSheet("font-size:13px;font-weight:600;margin-top:6px;");
    m_liveCard->body()->addWidget(m_flowLabel);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setStyleSheet(noteStyle);
    m_liveCard->body()->addWidget(m_note);

    lay->addWidget(m_liveCard);
    lay->addStretch();

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &PowerTestModule::onTick);

    retranslate();
}

// Starts the 1 Hz timer and does an immediate refresh.
void PowerTestModule::moduleEntered() {
    m_timer->start();
    onTick();
}

// Stops the timer.
void PowerTestModule::moduleLeft() {
    m_timer->stop();
}

// Reads PowerMetrics and updates source (AC/battery), charge state, V/A/W tiles and
// the explanation of the headline wattage.
void PowerTestModule::onTick() {
    const auto &pal = TesterTheme::current();
    const PowerReading r = PowerMetrics::read();

    // --- Source line ---
    // AC line status is authoritative for the source; fall back to the battery's
    // discharge flag if the OS reports the AC state as unknown.
    bool onAc = r.acKnown ? r.onAc : (r.hasBattery ? !r.discharging : true);
    m_sourceLabel->setText(onAc ? tr_("ht_power_on_ac") : tr_("ht_power_on_battery"));
    m_sourceLabel->setStyleSheet(QString("font-size:22px;font-weight:800;color:%1;")
                                     .arg((onAc ? pal.ok : pal.warn).name()));

    if (!r.hasBattery) {
        m_stateLabel->setText(tr_("ht_power_no_battery"));
    } else {
        QString state;
        if (r.charging)         state = tr_("ht_power_charging");
        else if (r.discharging) state = tr_("ht_power_discharging");
        else                    state = tr_("ht_power_not_charging");
        if (r.percent >= 0)
            state += "  ·  " + tr_("ht_power_battery_level")
                                   .replace("{pct}", QString::number(r.percent));
        m_stateLabel->setText(state);
    }

    // --- Headline: whole-system live power draw ---
    // Meaningful only on battery: with the charger unplugged, the battery's
    // discharge rate equals everything the computer is consuming right now. On
    // AC, Windows does not expose the adapter's total output, so we say so
    // instead of showing a misleading number.
    if (r.hasBattery && r.discharging && r.rateKnown) {
        m_drawVal->setText(QString::number(std::abs(r.powerW), 'f', 1) + " W");
        m_drawVal->setStyleSheet(QString("font-size:40px;font-weight:900;color:%1;")
                                     .arg(pal.textPrimary.name()));
        m_drawExplain->setText(tr_("ht_power_draw_battery_explain"));
    } else {
        m_drawVal->setText(tr_("ht_power_unavailable"));
        m_drawVal->setStyleSheet(QString("font-size:40px;font-weight:900;color:%1;")
                                     .arg(pal.textSecondary.name()));
        if (r.hasBattery && r.charging && r.rateKnown)
            m_drawExplain->setText(tr_("ht_power_draw_ac_charging_explain")
                                       .replace("{w}", QString::number(std::abs(r.powerW), 'f', 1)));
        else
            m_drawExplain->setText(tr_("ht_power_draw_ac_idle_explain"));
    }

    // --- Live V / A / W ---
    const QString na = tr_("ht_power_unavailable");
    auto fmt = [](double val, int prec, const char *unit) {
        return QString::number(val, 'f', prec) + " " + unit;
    };

    if (r.hasBattery && r.voltageKnown)
        m_voltageVal->setText(fmt(r.voltageV, 2, "V"));
    else
        m_voltageVal->setText(na);

    if (r.hasBattery && r.rateKnown && r.voltageKnown)
        m_currentVal->setText(fmt(std::abs(r.currentA), 2, "A"));
    else
        m_currentVal->setText(na);

    if (r.hasBattery && r.rateKnown)
        m_powerVal->setText(fmt(std::abs(r.powerW), 1, "W"));
    else
        m_powerVal->setText(na);

    // --- Flow description ---
    if (r.hasBattery && r.rateKnown) {
        const QString wattTxt = QString::number(std::abs(r.powerW), 'f', 1);
        if (r.charging) {
            m_flowLabel->setStyleSheet(QString("font-size:13px;font-weight:700;color:%1;")
                                           .arg(pal.ok.name()));
            m_flowLabel->setText(tr_("ht_power_flow_charging").replace("{w}", wattTxt));
        } else if (r.discharging) {
            m_flowLabel->setStyleSheet(QString("font-size:13px;font-weight:700;color:%1;")
                                           .arg(pal.warn.name()));
            m_flowLabel->setText(tr_("ht_power_flow_discharging").replace("{w}", wattTxt));
        } else {
            m_flowLabel->setStyleSheet(QString("font-size:13px;font-weight:600;color:%1;")
                                           .arg(pal.textSecondary.name()));
            m_flowLabel->setText(tr_("ht_power_flow_idle"));
        }
    } else {
        m_flowLabel->setStyleSheet(QString("font-size:13px;color:%1;")
                                       .arg(pal.textSecondary.name()));
        m_flowLabel->setText(QString());
    }
}

// Re-applies translated texts.
void PowerTestModule::retranslate() {
    m_infoCard->setTitle(tr_("ht_power_intro_title"));
    m_sourceCard->setTitle(tr_("ht_power_source_title"));
    m_liveCard->setTitle(tr_("ht_power_live_title"));
    m_intro->setText(tr_("ht_power_intro"));
    m_drawCap->setText(tr_("ht_power_draw_label"));
    m_voltageCap->setText(tr_("ht_power_voltage"));
    m_currentCap->setText(tr_("ht_power_current"));
    m_powerCap->setText(tr_("ht_power_power"));
    m_note->setText(tr_("ht_power_note"));
    // Refresh live text immediately so a language switch re-labels the source line.
    if (m_timer && m_timer->isActive())
        onTick();
}
