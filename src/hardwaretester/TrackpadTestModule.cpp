// =============================================================================
// TrackpadTestModule.cpp - trackpad test page.
// =============================================================================
#include "TrackpadTestModule.h"
#include "common/Card.h"
#include "common/RawInputManager.h"
#include "common/TesterTheme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

// ── TrackpadDiagram ────────────────────────────────────────────

// Painted trackpad surface with two click zones.
TrackpadDiagram::TrackpadDiagram(QWidget *parent) : QWidget(parent) {
    setMinimumSize(190, 150);
}

// Press/release highlight; a pressed zone is remembered as tested.
void TrackpadDiagram::setButtonState(int button, bool pressed) {
    if (button < 0 || button > 1)
        return;
    m_pressed[button] = pressed;
    if (pressed)
        m_tested[button] = true;
    update();
}

// Forget pressed/tested state.
void TrackpadDiagram::reset() {
    for (int i = 0; i < 2; ++i)
        m_pressed[i] = m_tested[i] = false;
    update();
}

// Draws the surface and the zones with state colours.
void TrackpadDiagram::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto &pal = TesterTheme::current();

    auto fillFor = [&](int i) -> QColor {
        if (m_pressed[i]) return pal.accent.darker(130);
        if (m_tested[i]) return pal.accentSoft;
        return pal.canvasBg;
    };

    const QRectF pad = rect().adjusted(6, 4, -6, -4);
    const qreal zoneH = pad.height() * 0.28;

    // Surface.
    p.setPen(QPen(pal.cardBorder, 1.5));
    p.setBrush(pal.cardBg);
    p.drawRoundedRect(pad, 12, 12);

    // Bottom click zones (visual convention; taps register anywhere).
    QRectF leftZone(pad.left() + 3, pad.bottom() - zoneH, pad.width() / 2 - 4, zoneH - 3);
    QRectF rightZone(pad.left() + pad.width() / 2 + 1, pad.bottom() - zoneH,
                     pad.width() / 2 - 4, zoneH - 3);
    p.setBrush(fillFor(0));
    p.drawRoundedRect(leftZone, 8, 8);
    p.setBrush(fillFor(1));
    p.drawRoundedRect(rightZone, 8, 8);

    p.setPen(pal.textSecondary);
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.drawText(leftZone, Qt::AlignCenter, "L");
    p.drawText(rightZone, Qt::AlignCenter, "R");
}

// ── TrackpadTrailCanvas ────────────────────────────────────────

// Trail canvas; context menu disabled.
TrackpadTrailCanvas::TrackpadTrailCanvas(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setContextMenuPolicy(Qt::NoContextMenu); // two-finger tap is a test input here
    setMinimumHeight(140);
}

// Remove all points.
void TrackpadTrailCanvas::clearTrail() {
    m_points.clear();
    update();
}

// Appends the pointer position to the (length-limited) trail.
void TrackpadTrailCanvas::mouseMoveEvent(QMouseEvent *event) {
    m_points << event->position();
    if (m_points.size() > 2000)
        m_points.remove(0, m_points.size() - 2000);
    update();
}

// Draws the trail with fading alpha.
void TrackpadTrailCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto &pal = TesterTheme::current();
    p.fillRect(rect(), pal.canvasBg);
    p.setPen(QPen(pal.cardBorder, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    if (m_points.size() < 2)
        return;
    const int n = m_points.size();
    for (int i = 1; i < n; ++i) {
        QColor c = pal.accent;
        c.setAlphaF(0.15 + 0.85 * i / n);
        p.setPen(QPen(c, 2));
        p.drawLine(m_points[i - 1], m_points[i]);
    }
}

// ── TrackpadTestModule ─────────────────────────────────────────

// Precision touchpads register a HID top-level collection with usage page
// 0x0D (Digitizer), usage 0x05 (Touch Pad).
// Scans the Raw Input device list for a HID device with usage page 0x0D / usage 0x05
// (Digitizers / Touch Pad), i.e. a precision touchpad.
bool TrackpadTestModule::detectPrecisionTouchpad() {
    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 ||
        count == 0)
        return false;
    std::vector<RAWINPUTDEVICELIST> devices(count);
    count = GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST));
    if (count == UINT(-1))
        return false;
    for (UINT i = 0; i < count; ++i) {
        if (devices[i].dwType != RIM_TYPEHID)
            continue;
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        if (GetRawInputDeviceInfoW(devices[i].hDevice, RIDI_DEVICEINFO, &info, &size) > 0 &&
            info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05)
            return true;
    }
    return false;
}

TrackpadTestModule::TrackpadTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    m_clock.start();

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // Left column: device presence + click zones.
    auto *left = new QVBoxLayout();
    left->setSpacing(10);

    m_deviceCard = new Card(QString(), this);
    m_deviceLabel = new QLabel(this);
    m_deviceLabel->setWordWrap(true);
    m_deviceCard->body()->addWidget(m_deviceLabel);
    left->addWidget(m_deviceCard);

    m_clickCard = new Card(QString(), this);
    m_diagram = new TrackpadDiagram(this);
    m_clickCard->body()->addWidget(m_diagram, 1);
    m_clickHint = new QLabel(this);
    m_clickHint->setWordWrap(true);
    m_clickHint->setStyleSheet(QString("color:%1;font-size:11px;")
                                   .arg(TesterTheme::current().textSecondary.name()));
    m_clickCard->body()->addWidget(m_clickHint);
    m_resetBtn = new QPushButton(this);
    m_clickCard->body()->addWidget(m_resetBtn);
    left->addWidget(m_clickCard, 1);
    outer->addLayout(left, 1);

    // Right column: double-tap, scroll, report rate, trail.
    auto *right = new QVBoxLayout();
    right->setSpacing(10);

    auto *row1 = new QHBoxLayout();
    row1->setSpacing(10);
    m_dblCard = new Card(QString(), this);
    m_doubleTapLabel = new QLabel(this);
    m_doubleTapLabel->setTextFormat(Qt::RichText);
    m_doubleTapLabel->setWordWrap(true);
    m_dblCard->body()->addWidget(m_doubleTapLabel);
    m_dblCard->body()->addStretch();
    row1->addWidget(m_dblCard, 1);

    m_scrollCard = new Card(QString(), this);
    m_scrollLabel = new QLabel(this);
    m_scrollLabel->setTextFormat(Qt::RichText);
    m_scrollCard->body()->addWidget(m_scrollLabel);
    m_scrollCard->body()->addStretch();
    row1->addWidget(m_scrollCard, 1);
    right->addLayout(row1);

    m_rateCard = new Card(QString(), this);
    m_rateLabel = new QLabel(this);
    m_rateLabel->setTextFormat(Qt::RichText);
    m_rateCard->body()->addWidget(m_rateLabel);
    m_rateNote = new QLabel(this);
    m_rateNote->setWordWrap(true);
    m_rateNote->setStyleSheet(QString("color:%1;font-size:11px;")
                                  .arg(TesterTheme::current().textSecondary.name()));
    m_rateCard->body()->addWidget(m_rateNote);
    right->addWidget(m_rateCard);

    m_trailCard = new Card(QString(), this);
    m_trail = new TrackpadTrailCanvas(this);
    m_trailCard->body()->addWidget(m_trail, 1);
    m_clearTrailBtn = new QPushButton(this);
    connect(m_clearTrailBtn, &QPushButton::clicked, m_trail,
            &TrackpadTrailCanvas::clearTrail);
    m_trailCard->body()->addWidget(m_clearTrailBtn);
    right->addWidget(m_trailCard, 1);

    outer->addLayout(right, 2);

    connect(m_resetBtn, &QPushButton::clicked, this, &TrackpadTestModule::onReset);
    auto &rim = RawInputManager::instance();
    connect(&rim, &RawInputManager::mouseButton, this, &TrackpadTestModule::onButton);
    connect(&rim, &RawInputManager::mouseWheel, this, &TrackpadTestModule::onWheel);

    m_rateTimer = new QTimer(this);
    m_rateTimer->setInterval(100);
    connect(m_rateTimer, &QTimer::timeout, this, &TrackpadTestModule::updateRateLabels);

    retranslate();
}

// Acquires Raw Input for the mouse and starts the rate timer.
void TrackpadTestModule::moduleEntered() {
    m_precisionPad = detectPrecisionTouchpad();
    m_deviceLabel->setText(tr_(m_precisionPad ? "ht_tp_precision_yes"
                                              : "ht_tp_precision_no"));
    RawInputManager::instance().acquireMouse();
    m_rateTimer->start();
}

// Releases Raw Input and stops the timer.
void TrackpadTestModule::moduleLeft() {
    m_rateTimer->stop();
    RawInputManager::instance().releaseMouse();
}

// Marks click zones tested and measures double-tap intervals.
void TrackpadTestModule::onButton(int button, bool pressed) {
    if (!isVisible())
        return;
    if (button > 1)
        return; // trackpads report L/R only; other buttons come from mice
    m_diagram->setButtonState(button, pressed);
    if (pressed)
        m_buttonTested[button] = true;

    // Double-tap interval: consecutive left presses.
    if (button == 0 && pressed) {
        const qint64 now = m_clock.nsecsElapsed() / 1000000;
        if (m_lastLeftPress >= 0 && now - m_lastLeftPress < 2000)
            m_lastDblInterval = now - m_lastLeftPress;
        m_lastLeftPress = now;

        const UINT threshold = GetDoubleClickTime();
        if (m_lastDblInterval >= 0) {
            const bool pass = m_lastDblInterval <= qint64(threshold);
            const auto &pal = TesterTheme::current();
            m_doubleTapLabel->setText(
                QString("<span style='font-size:22px;font-weight:700;color:%1'>%2 ms</span>"
                        "<br>%3: %4 ms")
                    .arg(pass ? pal.ok.name() : pal.warn.name())
                    .arg(m_lastDblInterval)
                    .arg(tr_("ht_ms_dbl_threshold"))
                    .arg(threshold));
        }
    }
    updateResult();
}

// Counts scroll events per direction.
void TrackpadTestModule::onWheel(bool horizontal, int delta) {
    if (!isVisible())
        return;
    const int notches = qMax(1, qAbs(delta) / 120);
    if (!horizontal)
        m_scrollCounts[delta > 0 ? 0 : 1] += notches;
    else
        m_scrollCounts[delta > 0 ? 3 : 2] += notches;
    updateScrollLabel();
    updateResult();
}

// Shows scroll counters.
void TrackpadTestModule::updateScrollLabel() {
    m_scrollLabel->setText(QString("↑ %1 &nbsp;&nbsp; ↓ %2<br>← %3 &nbsp;&nbsp; → %4")
                               .arg(m_scrollCounts[0]).arg(m_scrollCounts[1])
                               .arg(m_scrollCounts[2]).arg(m_scrollCounts[3]));
}

// Shows report-rate statistics.
void TrackpadTestModule::updateRateLabels() {
    auto &rim = RawInputManager::instance();
    const double cur = rim.currentRate(100);
    const double peak = rim.peakRate();
    const double sustained = rim.sustainedRate();
    m_rateLabel->setText(
        QString("<span style='font-size:26px;font-weight:700;color:%1'>%2 Hz</span>"
                "<br>%3: %4 Hz &nbsp;&nbsp; %5: %6 Hz")
            .arg(TesterTheme::current().accent.name())
            .arg(qRound(cur))
            .arg(tr_("ht_ms_poll_peak")).arg(qRound(peak))
            .arg(tr_("ht_ms_poll_sustained")).arg(qRound(sustained)));
}

// Writes the summary for the report.
void TrackpadTestModule::updateResult() {
    const int tested = (m_buttonTested[0] ? 1 : 0) + (m_buttonTested[1] ? 1 : 0);
    const int scrollDirs = (m_scrollCounts[0] > 0) + (m_scrollCounts[1] > 0) +
                           (m_scrollCounts[2] > 0) + (m_scrollCounts[3] > 0);
    QString summary = QString("%1, taps %2/2, scroll %3/4")
                          .arg(m_precisionPad ? "precision" : "standard")
                          .arg(tested).arg(scrollDirs);
    const double peak = RawInputManager::instance().peakRate();
    if (peak > 0)
        summary += QString(", report peak %1 Hz").arg(qRound(peak));
    recordResult("ht_tp_result_name", summary);
}

// Clears test state.
void TrackpadTestModule::onReset() {
    m_diagram->reset();
    for (bool &b : m_buttonTested)
        b = false;
    for (int &c : m_scrollCounts)
        c = 0;
    m_lastLeftPress = -1;
    m_lastDblInterval = -1;
    m_trail->clearTrail();
    RawInputManager::instance().resetMouseStats();
    retranslate();
    updateResult();
}

// Re-applies translated texts.
void TrackpadTestModule::retranslate() {
    m_deviceCard->setTitle(tr_("ht_tp_device_title"));
    m_clickCard->setTitle(tr_("ht_tp_click_title"));
    m_dblCard->setTitle(tr_("ht_tp_dbl_title"));
    m_scrollCard->setTitle(tr_("ht_tp_scroll_title"));
    m_rateCard->setTitle(tr_("ht_tp_rate_title"));
    m_trailCard->setTitle(tr_("ht_tp_trail_title"));
    m_clickHint->setText(tr_("ht_tp_click_hint"));
    m_resetBtn->setText(tr_("ht_common_reset"));
    m_clearTrailBtn->setText(tr_("ht_ms_clear_trail"));
    m_rateNote->setText(tr_("ht_tp_rate_note"));
    m_deviceLabel->setText(tr_(m_precisionPad ? "ht_tp_precision_yes"
                                              : "ht_tp_precision_no"));
    if (m_lastDblInterval < 0)
        m_doubleTapLabel->setText(tr_("ht_tp_dbl_hint"));
    updateScrollLabel();
    updateRateLabels();
}
