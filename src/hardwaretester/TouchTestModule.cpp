// =============================================================================
// TouchTestModule.cpp - touchscreen test page logic.
// =============================================================================
#include "TouchTestModule.h"
#include "../TouchTestWidget.h"
#include "common/Card.h"
#include "common/TesterTheme.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

TouchTestModule::TouchTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 18, 22, 18);
    outer->setSpacing(12);

    m_usageLabel = new QLabel(this);
    m_usageLabel->setWordWrap(true);
    m_usageLabel->setStyleSheet(QString("color:%1;font-size:12px;")
                                    .arg(TesterTheme::current().textSecondary.name()));
    outer->addWidget(m_usageLabel);

    auto *statusCard = new Card(QString(), this);
    m_deviceLabel = new QLabel(statusCard);
    m_deviceLabel->setWordWrap(true);
    statusCard->body()->addWidget(m_deviceLabel);
    outer->addWidget(statusCard);

    auto *controlRow = new QHBoxLayout();
    m_counterLabel = new QLabel(this);
    m_counterLabel->setStyleSheet(QString("color:%1;font-size:13px;font-weight:600;")
                                      .arg(TesterTheme::current().textPrimary.name()));
    controlRow->addWidget(m_counterLabel);
    controlRow->addStretch();

    // Keep the two controls compact — the canvas is what matters.
    const QString smallBtn =
        "QPushButton{background:#ecf0f1; border:1px solid #bdc3c7; border-radius:4px;"
        " padding:2px 10px; font-size:12px; color:#000;} QPushButton:hover{background:#dfe6e9;}";
    m_clearButton = new QPushButton(this);
    m_clearButton->setStyleSheet(smallBtn);
    m_clearButton->setFixedHeight(24);
    controlRow->addWidget(m_clearButton);
    m_fullScreenButton = new QPushButton(this);
    m_fullScreenButton->setStyleSheet(smallBtn);
    m_fullScreenButton->setFixedHeight(24);
    controlRow->addWidget(m_fullScreenButton);
    outer->addLayout(controlRow);

    m_canvas = new TouchCanvas(this);
    m_canvas->setMinimumHeight(340);
    outer->addWidget(m_canvas, 1);

    connect(m_clearButton, &QPushButton::clicked,
            m_canvas, &TouchCanvas::clearCanvas);
    connect(m_fullScreenButton, &QPushButton::clicked,
            this, &TouchTestModule::toggleFullScreen);
    connect(m_canvas, &TouchCanvas::touchInfo,
            this, &TouchTestModule::onTouchInfo);

    updateDeviceStatus();
    retranslate();
}

// Asks GetSystemMetrics whether a touch digitizer is ready and how many contacts
// it supports; sets m_touchAvailable / m_reportedCapacity.
void TouchTestModule::updateDeviceStatus() {
    m_touchAvailable = false;
    m_reportedCapacity = 0;
#ifdef Q_OS_WIN
    const int digitizer = GetSystemMetrics(SM_DIGITIZER);
    const bool ready = (digitizer & NID_READY) != 0;
    const bool touch = (digitizer & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) != 0;
    m_reportedCapacity = GetSystemMetrics(SM_MAXIMUMTOUCHES);
    m_touchAvailable = (ready && touch) || m_reportedCapacity > 0;
#endif
}

// Refreshes device status on entry.
void TouchTestModule::moduleEntered() {
    updateDeviceStatus();
    retranslate();
}

// Canvas slot: updates the live/maximum contact counters and the report result
// (only real touch contacts count, not mouse input).
void TouchTestModule::onTouchInfo(int active, int maximum) {
    m_activeTouches = active;
    m_maxTouches = qMax(m_maxTouches, maximum);
    m_counterLabel->setText(tr_("ht_touch_counter")
                                .replace("{active}", QString::number(m_activeTouches))
                                .replace("{max}", QString::number(m_maxTouches)));
    if (m_maxTouches > 0) {
        recordResult("ht_touch_result_name",
                     tr_("ht_touch_result")
                         .replace("{max}", QString::number(m_maxTouches)));
    }
}

// Enters/leaves the dedicated full-screen test surface with small Reset/Exit buttons.
void TouchTestModule::toggleFullScreen() {
    if (m_fsOverlay) {          // already full-screen → exit
        m_fsOverlay->close();   // WA_DeleteOnClose; the destroyed handler clears the pointers
        return;
    }

    // A dedicated full-screen window whose canvas fills the ENTIRE screen, so
    // every pixel of the digitizer — edges and corners included — is inside the
    // drawing surface. Only two tiny Reset/Exit buttons float on top.
    auto *overlay = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint);
    overlay->setAttribute(Qt::WA_DeleteOnClose);
    overlay->setStyleSheet("background:#ffffff;");
    m_fsOverlay = overlay;

    auto *lay = new QVBoxLayout(overlay);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *canvas = new TouchCanvas(overlay);
    lay->addWidget(canvas);
    connect(canvas, &TouchCanvas::touchInfo, this, &TouchTestModule::onTouchInfo);

    // Minimal floating controls, kept as small as possible so nearly the whole
    // screen stays testable.
    auto *ctl = new QWidget(overlay);
    auto *cl = new QHBoxLayout(ctl);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(4);
    const QString tiny =
        "QPushButton{background:rgba(236,240,241,0.90); border:1px solid #bdc3c7;"
        " border-radius:4px; padding:1px 7px; font-size:11px; color:#000;}"
        " QPushButton:hover{background:#dfe6e9;}";
    auto *resetBtn = new QPushButton(tr_("ht_touch_clear"), ctl);
    auto *exitBtn = new QPushButton(tr_("ht_touch_exit_fullscreen"), ctl);
    for (QPushButton *b : {resetBtn, exitBtn}) {
        b->setStyleSheet(tiny);
        b->setFixedHeight(20);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
    }
    cl->addWidget(resetBtn);
    cl->addWidget(exitBtn);
    m_fsButtons = ctl;

    connect(resetBtn, &QPushButton::clicked, canvas, &TouchCanvas::clearCanvas);
    connect(exitBtn, &QPushButton::clicked, overlay, &QWidget::close);
    connect(overlay, &QObject::destroyed, this, [this]() {
        m_fsOverlay = nullptr;
        m_fsButtons = nullptr;
    });

    overlay->installEventFilter(this);
    overlay->showFullScreen();

    // Now that the screen-sized geometry is known, tuck the controls into the
    // top-right corner, above the canvas.
    ctl->adjustSize();
    ctl->move(overlay->width() - ctl->width() - 8, 8);
    ctl->raise();
    overlay->setFocus(); // so Esc reaches the event filter
}

// Keeps the overlay buttons positioned when the full-screen window resizes.
bool TouchTestModule::eventFilter(QObject *obj, QEvent *event) {
    if (obj == m_fsOverlay) {
        if (event->type() == QEvent::Resize && m_fsButtons) {
            m_fsButtons->adjustSize();
            m_fsButtons->move(m_fsOverlay->width() - m_fsButtons->width() - 8, 8);
            m_fsButtons->raise();
        } else if (event->type() == QEvent::KeyPress &&
                   static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            m_fsOverlay->close();
            return true;
        }
    }
    return TesterModule::eventFilter(obj, event);
}

// Leaves full-screen mode if active.
void TouchTestModule::moduleLeft() {
    if (m_fsOverlay)
        m_fsOverlay->close(); // don't leave a stray full-screen window behind
}

// Re-applies translated texts.
void TouchTestModule::retranslate() {
    m_usageLabel->setText(tr_("ht_touch_usage"));
    if (m_touchAvailable) {
        m_deviceLabel->setText(tr_("ht_touch_detected")
                                   .replace("{count}", QString::number(m_reportedCapacity)));
        m_deviceLabel->setStyleSheet(QString("color:%1;font-weight:600;")
                                         .arg(TesterTheme::current().ok.name()));
    } else {
        m_deviceLabel->setText(tr_("ht_touch_not_detected"));
        m_deviceLabel->setStyleSheet(QString("color:%1;")
                                         .arg(TesterTheme::current().warn.name()));
    }
    m_counterLabel->setText(tr_("ht_touch_counter")
                                .replace("{active}", QString::number(m_activeTouches))
                                .replace("{max}", QString::number(m_maxTouches)));
    m_clearButton->setText(tr_("ht_touch_clear"));
    m_fullScreenButton->setText(tr_("ht_touch_fullscreen"));
}
