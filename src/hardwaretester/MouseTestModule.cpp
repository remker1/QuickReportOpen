// =============================================================================
// MouseTestModule.cpp - mouse test page (buttons, wheel, polling rate, trail).
// =============================================================================
#include "MouseTestModule.h"
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

// ── MouseDiagram ───────────────────────────────────────────────

// Painted mouse with five hit zones.
MouseDiagram::MouseDiagram(QWidget *parent) : QWidget(parent) {
    setMinimumSize(170, 230);
}

// Press/release highlight; remembers tested buttons.
void MouseDiagram::setButtonState(int button, bool pressed) {
    if (button < 0 || button > 4)
        return;
    m_pressed[button] = pressed;
    if (pressed)
        m_tested[button] = true;
    update();
}

// Forget pressed/tested state.
void MouseDiagram::reset() {
    for (int i = 0; i < 5; ++i)
        m_pressed[i] = m_tested[i] = false;
    update();
}

// Draws the mouse body and button zones.
void MouseDiagram::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto &pal = TesterTheme::current();

    auto fillFor = [&](int i) -> QColor {
        if (m_pressed[i]) return pal.accent.darker(130);
        if (m_tested[i]) return pal.accentSoft;
        return pal.canvasBg;
    };

    // Body proportions inside the widget.
    const qreal w = width(), h = height();
    const QRectF body(w * 0.22, h * 0.06, w * 0.56, h * 0.88);
    const qreal split = body.top() + body.height() * 0.38; // buttons/palm split

    p.setPen(QPen(pal.cardBorder, 1.5));

    // Palm.
    QPainterPath palm;
    palm.addRoundedRect(body, w * 0.18, w * 0.18);
    p.setBrush(pal.cardBg);
    p.drawPath(palm);

    // Left / right buttons: top part of the body split vertically, with a
    // gap in the middle for the wheel.
    const qreal midW = body.width() * 0.20;
    QRectF leftBtn(body.left(), body.top(), (body.width() - midW) / 2, split - body.top());
    QRectF rightBtn(leftBtn.right() + midW, body.top(), leftBtn.width(), leftBtn.height());

    QPainterPath leftPath;
    leftPath.addRoundedRect(leftBtn.adjusted(2, 2, -1, -2), 10, 10);
    p.setBrush(fillFor(0));
    p.drawPath(leftPath);

    QPainterPath rightPath;
    rightPath.addRoundedRect(rightBtn.adjusted(1, 2, -2, -2), 10, 10);
    p.setBrush(fillFor(1));
    p.drawPath(rightPath);

    // Wheel (middle button).
    QRectF wheel(leftBtn.right() + midW * 0.15, body.top() + leftBtn.height() * 0.25,
                 midW * 0.7, leftBtn.height() * 0.6);
    p.setBrush(fillFor(2));
    p.drawRoundedRect(wheel, wheel.width() / 2, wheel.width() / 2);

    // Side buttons X1/X2 on the left flank.
    QRectF x2(body.left() - w * 0.10, h * 0.32, w * 0.09, h * 0.13);
    QRectF x1(body.left() - w * 0.10, h * 0.48, w * 0.09, h * 0.13);
    p.setBrush(fillFor(4));
    p.drawRoundedRect(x2, 4, 4);
    p.setBrush(fillFor(3));
    p.drawRoundedRect(x1, 4, 4);

    // Labels.
    p.setPen(pal.textSecondary);
    QFont f = p.font();
    f.setPointSizeF(8);
    p.setFont(f);
    p.drawText(leftBtn, Qt::AlignCenter, "L");
    p.drawText(rightBtn, Qt::AlignCenter, "R");
    p.drawText(QRectF(x1.left() - 2, x1.top(), x1.width() + 4, x1.height()),
               Qt::AlignCenter, "X1");
    p.drawText(QRectF(x2.left() - 2, x2.top(), x2.width() + 4, x2.height()),
               Qt::AlignCenter, "X2");
}

// ── MouseTrailCanvas ───────────────────────────────────────────

// Trail canvas; context menu disabled.
MouseTrailCanvas::MouseTrailCanvas(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setContextMenuPolicy(Qt::NoContextMenu); // right-click is a test input here
    setMinimumHeight(140);
}

// Remove all points.
void MouseTrailCanvas::clearTrail() {
    m_points.clear();
    update();
}

// Appends the pointer position to the trail.
void MouseTrailCanvas::mouseMoveEvent(QMouseEvent *event) {
    m_points << event->position();
    if (m_points.size() > 2000)
        m_points.remove(0, m_points.size() - 2000);
    update();
}

// Draws the fading trail.
void MouseTrailCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto &pal = TesterTheme::current();
    p.fillRect(rect(), pal.canvasBg);
    p.setPen(QPen(pal.cardBorder, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    if (m_points.size() < 2)
        return;
    // Older segments fade out.
    const int n = m_points.size();
    for (int i = 1; i < n; ++i) {
        QColor c = pal.accent;
        c.setAlphaF(0.15 + 0.85 * i / n);
        p.setPen(QPen(c, 2));
        p.drawLine(m_points[i - 1], m_points[i]);
    }
}

// ── MouseTestModule ────────────────────────────────────────────

MouseTestModule::MouseTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    m_clock.start();

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // Left column: diagram + buttons state.
    auto *left = new QVBoxLayout();
    m_diagramCard = new Card(QString(), this);
    m_diagram = new MouseDiagram(this);
    m_diagramCard->body()->addWidget(m_diagram, 1);
    m_buttonsLabel = new QLabel(this);
    m_buttonsLabel->setWordWrap(true);
    m_diagramCard->body()->addWidget(m_buttonsLabel);
    m_resetBtn = new QPushButton(this);
    m_diagramCard->body()->addWidget(m_resetBtn);
    left->addWidget(m_diagramCard, 1);
    outer->addLayout(left, 1);

    // Right column: double-click, wheel, polling, trail.
    auto *right = new QVBoxLayout();
    right->setSpacing(10);

    auto *row1 = new QHBoxLayout();
    row1->setSpacing(10);
    m_dblCard = new Card(QString(), this);
    m_doubleClickLabel = new QLabel(this);
    m_doubleClickLabel->setTextFormat(Qt::RichText);
    m_doubleClickLabel->setWordWrap(true);
    m_dblCard->body()->addWidget(m_doubleClickLabel);
    m_dblCard->body()->addStretch();
    row1->addWidget(m_dblCard, 1);

    m_wheelCard = new Card(QString(), this);
    m_wheelLabel = new QLabel(this);
    m_wheelLabel->setTextFormat(Qt::RichText);
    m_wheelCard->body()->addWidget(m_wheelLabel);
    m_wheelCard->body()->addStretch();
    row1->addWidget(m_wheelCard, 1);
    right->addLayout(row1);

    m_pollCard = new Card(QString(), this);
    m_pollLabel = new QLabel(this);
    m_pollLabel->setTextFormat(Qt::RichText);
    m_pollCard->body()->addWidget(m_pollLabel);
    m_pollNote = new QLabel(this);
    m_pollNote->setWordWrap(true);
    m_pollNote->setStyleSheet(QString("color:%1;font-size:11px;")
                                  .arg(TesterTheme::current().textSecondary.name()));
    m_pollCard->body()->addWidget(m_pollNote);
    right->addWidget(m_pollCard);

    m_trailCard = new Card(QString(), this);
    m_trail = new MouseTrailCanvas(this);
    m_trailCard->body()->addWidget(m_trail, 1);
    m_clearTrailBtn = new QPushButton(this);
    connect(m_clearTrailBtn, &QPushButton::clicked, m_trail, &MouseTrailCanvas::clearTrail);
    m_trailCard->body()->addWidget(m_clearTrailBtn);
    right->addWidget(m_trailCard, 1);

    outer->addLayout(right, 2);

    connect(m_resetBtn, &QPushButton::clicked, this, &MouseTestModule::onReset);
    auto &rim = RawInputManager::instance();
    connect(&rim, &RawInputManager::mouseButton, this, &MouseTestModule::onButton);
    connect(&rim, &RawInputManager::mouseWheel, this, &MouseTestModule::onWheel);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(100);
    connect(m_pollTimer, &QTimer::timeout, this, &MouseTestModule::updatePollingLabels);

    retranslate();
}

// Acquires Raw Input (mouse) and starts the polling-rate timer.
void MouseTestModule::moduleEntered() {
    RawInputManager::instance().acquireMouse();
    m_pollTimer->start();
}

// Releases Raw Input and stops the timer.
void MouseTestModule::moduleLeft() {
    m_pollTimer->stop();
    RawInputManager::instance().releaseMouse();
}

// Marks a button tested; left-button presses also measure the double-click interval.
void MouseTestModule::onButton(int button, bool pressed) {
    if (!isVisible())
        return;
    m_diagram->setButtonState(button, pressed);
    if (pressed && button >= 0 && button < 5)
        m_buttonTested[button] = true;

    // Double-click interval: consecutive left presses.
    if (button == 0 && pressed) {
        const qint64 now = m_clock.nsecsElapsed() / 1000000;
        if (m_lastLeftPress >= 0 && now - m_lastLeftPress < 2000)
            m_lastDblInterval = now - m_lastLeftPress;
        m_lastLeftPress = now;

        const UINT threshold = GetDoubleClickTime();
        if (m_lastDblInterval >= 0) {
            const bool pass = m_lastDblInterval <= qint64(threshold);
            const auto &pal = TesterTheme::current();
            m_doubleClickLabel->setText(
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

// Counts wheel notches per direction.
void MouseTestModule::onWheel(bool horizontal, int delta) {
    if (!isVisible())
        return;
    const int notches = qMax(1, qAbs(delta) / 120);
    if (!horizontal)
        m_wheelCounts[delta > 0 ? 0 : 1] += notches;
    else
        m_wheelCounts[delta > 0 ? 3 : 2] += notches; // HWHEEL: positive = right
    updateWheelLabel();
    updateResult();
}

// Shows wheel counters.
void MouseTestModule::updateWheelLabel() {
    m_wheelLabel->setText(QString("↑ %1 &nbsp;&nbsp; ↓ %2<br>← %3 &nbsp;&nbsp; → %4")
                              .arg(m_wheelCounts[0]).arg(m_wheelCounts[1])
                              .arg(m_wheelCounts[2]).arg(m_wheelCounts[3]));
}

// Shows current / peak / sustained polling rate from RawInputManager.
void MouseTestModule::updatePollingLabels() {
    auto &rim = RawInputManager::instance();
    const double cur = rim.currentRate(100);
    const double peak = rim.peakRate();
    const double sustained = rim.sustainedRate();
    m_pollLabel->setText(
        QString("<span style='font-size:26px;font-weight:700;color:%1'>%2 Hz</span>"
                "<br>%3: %4 Hz &nbsp;&nbsp; %5: %6 Hz")
            .arg(TesterTheme::current().accent.name())
            .arg(qRound(cur))
            .arg(tr_("ht_ms_poll_peak")).arg(qRound(peak))
            .arg(tr_("ht_ms_poll_sustained")).arg(qRound(sustained)));
}

// Writes the summary for the report.
void MouseTestModule::updateResult() {
    int tested = 0;
    for (bool b : m_buttonTested)
        tested += b ? 1 : 0;
    const int wheelDirs = (m_wheelCounts[0] > 0) + (m_wheelCounts[1] > 0) +
                          (m_wheelCounts[2] > 0) + (m_wheelCounts[3] > 0);
    QString summary = QString("%1/5 buttons, wheel %2/4").arg(tested).arg(wheelDirs);
    const double peak = RawInputManager::instance().peakRate();
    if (peak > 0)
        summary += QString(", polling peak %1 Hz").arg(qRound(peak));
    recordResult("ht_ms_result_name", summary);
}

// Clears test state and statistics.
void MouseTestModule::onReset() {
    m_diagram->reset();
    for (bool &b : m_buttonTested)
        b = false;
    for (int &c : m_wheelCounts)
        c = 0;
    m_lastLeftPress = -1;
    m_lastDblInterval = -1;
    m_trail->clearTrail();
    RawInputManager::instance().resetMouseStats();
    retranslate();
    updateResult();
}

// Re-applies translated texts.
void MouseTestModule::retranslate() {
    m_diagramCard->setTitle(tr_("ht_ms_buttons_title"));
    m_dblCard->setTitle(tr_("ht_ms_dbl_title"));
    m_wheelCard->setTitle(tr_("ht_ms_wheel_title"));
    m_pollCard->setTitle(tr_("ht_ms_poll_title"));
    m_trailCard->setTitle(tr_("ht_ms_trail_title"));
    m_buttonsLabel->setText(tr_("ht_ms_buttons_hint"));
    m_resetBtn->setText(tr_("ht_common_reset"));
    m_clearTrailBtn->setText(tr_("ht_ms_clear_trail"));
    m_pollNote->setText(tr_("ht_ms_poll_note"));
    if (m_lastDblInterval < 0)
        m_doubleClickLabel->setText(tr_("ht_ms_dbl_hint"));
    updateWheelLabel();
    updatePollingLabels();
}
