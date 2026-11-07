// =============================================================================
// TrackpadTestModule.h - trackpad test page.
// =============================================================================
#ifndef TRACKPADTESTMODULE_H
#define TRACKPADTESTMODULE_H

#include "TesterModule.h"

#include <QElapsedTimer>
#include <QPolygonF>

class QLabel;
class QPushButton;
class Card;

// Painted trackpad surface: tracking area with two click zones at the bottom
// (left click / right click, i.e. one- and two-finger tap), driven by Raw
// Input button events.
class TrackpadDiagram : public QWidget {
    Q_OBJECT
public:
    explicit TrackpadDiagram(QWidget *parent = nullptr);
    void setButtonState(int button, bool pressed); // 0=L 1=R
    void reset();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool m_pressed[2] = {};
    bool m_tested[2] = {};
};

// Fading pointer trail; doubles as the interaction test area (no context menu
// so a two-finger tap can be tested without a popup).
class TrackpadTrailCanvas : public QWidget {
    Q_OBJECT
public:
    explicit TrackpadTrailCanvas(QWidget *parent = nullptr);
    void clearTrail();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QPolygonF m_points;
};

// Trackpad test. Windows does not expose a precision touchpad's raw
// multi-touch contacts to normal applications (they are reserved for system
// gestures), so this module honestly tests what the OS delivers: pointer
// motion, one-/two-finger taps (left/right button), two-finger scrolling,
// double-tap interval and the hardware report rate — and states whether a
// precision touchpad is present at all.
class TrackpadTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit TrackpadTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    // Raw Input slot: click zone tested / double-tap interval.
    void onButton(int button, bool pressed);
    // Raw Input slot: two-finger scroll direction counters.
    void onWheel(bool horizontal, int delta);
    // Timer slot: shows the report rate.
    void updateRateLabels();
    // Shows the scroll counters.
    void updateScrollLabel();
    // Writes the summary used by the report.
    void updateResult();
    // Clears all test state.
    void onReset();
    // True if Windows reports a precision touchpad device.
    static bool detectPrecisionTouchpad();

    TrackpadDiagram *m_diagram;
    TrackpadTrailCanvas *m_trail;
    QLabel *m_deviceLabel;
    QLabel *m_clickHint;
    QLabel *m_doubleTapLabel;
    QLabel *m_scrollLabel;
    QLabel *m_rateLabel;
    QLabel *m_rateNote;
    QPushButton *m_resetBtn;
    QPushButton *m_clearTrailBtn;
    Card *m_deviceCard, *m_clickCard, *m_dblCard, *m_scrollCard, *m_rateCard, *m_trailCard;
    class QTimer *m_rateTimer;

    bool m_buttonTested[2] = {};
    int m_scrollCounts[4] = {}; // up, down, left, right
    QElapsedTimer m_clock;
    qint64 m_lastLeftPress = -1;
    qint64 m_lastDblInterval = -1;
    bool m_precisionPad = false;
};

#endif // TRACKPADTESTMODULE_H
