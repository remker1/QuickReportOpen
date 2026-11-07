// =============================================================================
// InputTestWidget.h - LEGACY Qt-event keyboard/trackpad test.
//
// Not part of the current CMake target; kept for reference.
// =============================================================================
#ifndef INPUTTESTWIDGET_H
#define INPUTTESTWIDGET_H

#include <QWidget>
#include <QLabel>
#include <QMap>
#include <QList>
#include <QPair>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>

class QResizeEvent;

// One key cap of the legacy on-screen keyboard (default / pressed / visited styles).
class KeyLabel : public QLabel {
    Q_OBJECT
public:
    explicit KeyLabel(const QString &text, QWidget *parent = nullptr);
    void setActive(bool active);

private:
    bool isVisited;
    QString defaultStyle;
    QString pressedStyle;
    QString visitedStyle;
};

// Numpad keys share key codes with the main keyboard (e.g. Key_5). We OR this
// flag in so numpad and main keys are tracked independently.
static const int KEYPAD_FLAG = 0x40000000;

// Legacy trackpad area that reacts to mouse move/click/wheel events.
class TrackpadLabel : public QLabel {
    Q_OBJECT
public:
    explicit TrackpadLabel(const QString &text, QWidget *parent = nullptr);

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QString defaultStyle;
    QString activeStyle;
};

// Legacy keyboard + trackpad test built on Qt key events (superseded by
// KeyboardTestModule / TrackpadTestModule, which use Raw Input).
class InputTestWidget : public QWidget {
    Q_OBJECT
public:
    explicit InputTestWidget(QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void setupUi();
    void handleKey(int key, bool pressed, bool keypad);
    void relayoutKeys();

    QMap<int, QList<KeyLabel*>> keyMap;
    QList<QPair<KeyLabel*, int>> m_keys; // (key, logical width at base size 46)
    TrackpadLabel *trackpadLabel;
};

#endif // INPUTTESTWIDGET_H
