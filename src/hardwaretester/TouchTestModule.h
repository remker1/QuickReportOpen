// =============================================================================
// TouchTestModule.h - touchscreen test page (canvas from TouchTestWidget.h).
// =============================================================================
#ifndef TOUCHTESTMODULE_H
#define TOUCHTESTMODULE_H

#include "TesterModule.h"

class QLabel;
class QPushButton;
class TouchCanvas;

// Touchscreen digitizer test. The canvas consumes Qt touch events directly,
// draws one colored trail per contact and reports the maximum simultaneous
// physical contacts observed. Mouse input remains a drawing fallback but is
// deliberately not counted as a successful touchscreen result.
class TouchTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit TouchTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

protected:
    // Repositions the tiny overlay controls when the full-screen window resizes.
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void updateDeviceStatus();
    void onTouchInfo(int active, int maximum);
    void toggleFullScreen();

    QLabel *m_usageLabel;
    QLabel *m_deviceLabel;
    QLabel *m_counterLabel;
    QPushButton *m_clearButton;
    QPushButton *m_fullScreenButton;
    TouchCanvas *m_canvas;
    // Dedicated full-screen test surface: a canvas that fills the whole screen
    // (so every edge/corner of the digitizer is reachable) plus two tiny
    // Reset/Exit buttons. Null when not in full-screen mode.
    QWidget *m_fsOverlay = nullptr;
    QWidget *m_fsButtons = nullptr;
    int m_activeTouches = 0;
    int m_maxTouches = 0;
    int m_reportedCapacity = 0;
    bool m_touchAvailable = false;
};

#endif // TOUCHTESTMODULE_H
