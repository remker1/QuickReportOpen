// =============================================================================
// MouseTestModule.h - mouse test page.
// =============================================================================
#ifndef MOUSETESTMODULE_H
#define MOUSETESTMODULE_H

#include "TesterModule.h"

#include <QElapsedTimer>
#include <QPolygonF>

class QLabel;
class QPushButton;
class Card;

// Painted mouse silhouette with five hit zones (L/R/M + X1/X2 side buttons),
// driven by Raw Input events, not widget clicks.
class MouseDiagram : public QWidget {
    Q_OBJECT
public:
    explicit MouseDiagram(QWidget *parent = nullptr);
    void setButtonState(int button, bool pressed); // 0=L 1=R 2=M 3=X1 4=X2
    void reset();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool m_pressed[5] = {};
    bool m_tested[5] = {};
};

// Fading movement trail; also serves as the click test area (no context menu).
class MouseTrailCanvas : public QWidget {
    Q_OBJECT
public:
    explicit MouseTrailCanvas(QWidget *parent = nullptr);
    void clearTrail();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QPolygonF m_points;
};

// Mouse test page: button diagram, double-click interval, wheel directions,
// polling rate (from Raw Input statistics) and a movement trail.
class MouseTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit MouseTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    // Raw Input slot: marks a button tested; measures the left double-click interval.
    void onButton(int button, bool pressed);
    // Raw Input slot: counts wheel notches per direction.
    void onWheel(bool horizontal, int delta);
    // Timer slot: shows current/peak/sustained report rate.
    void updatePollingLabels();
    // Shows the up/down/left/right wheel counters.
    void updateWheelLabel();
    // Writes the summary line used by the report (recordResult).
    void updateResult();
    // Clears all test state.
    void onReset();

    MouseDiagram *m_diagram;
    MouseTrailCanvas *m_trail;
    QLabel *m_buttonsLabel;
    QLabel *m_doubleClickLabel;
    QLabel *m_wheelLabel;
    QLabel *m_pollLabel;
    QLabel *m_pollNote;
    QPushButton *m_resetBtn;
    QPushButton *m_clearTrailBtn;
    Card *m_diagramCard, *m_dblCard, *m_wheelCard, *m_pollCard, *m_trailCard;
    class QTimer *m_pollTimer;

    bool m_buttonTested[5] = {};
    int m_wheelCounts[4] = {}; // up, down, left, right
    QElapsedTimer m_clock;
    qint64 m_lastLeftPress = -1;
    qint64 m_lastDblInterval = -1;
};

#endif // MOUSETESTMODULE_H
