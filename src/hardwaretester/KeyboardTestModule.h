// =============================================================================
// KeyboardTestModule.h - keyboard test page (see class comments below).
// =============================================================================
#ifndef KEYBOARDTESTMODULE_H
#define KEYBOARDTESTMODULE_H

#include "TesterModule.h"

#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QList>
#include <QSet>

class QCheckBox;
class QFrame;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class QWidget;

// One key cap on the on-screen keyboard. Keys are identified by their
// physical identity: scanCode | e0<<8 | e1<<9 (so numpad-5 vs main-5 and
// left vs right modifiers are distinct, unlike Qt::Key).
class TesterKeyLabel : public QLabel {
public:
    // Creates a key cap showing the given text.
    explicit TesterKeyLabel(const QString &text, QWidget *parent = nullptr);
    // Pressed = highlighted; releasing keeps it marked as visited (tested).
    void setPressed(bool down);
    // Marks the key as having produced a chatter (double-trigger) event.
    void setChattered();
    // Back to the untested look.
    void reset();

private:
    // Re-applies the style sheet matching the current pressed/visited/chatter state.
    void applyStyle();
    bool m_pressed = false;
    bool m_visited = false;
    bool m_chattered = false;
};

// Keyboard test: full 104-key layout driven by Raw Input (captures PrtSc,
// Pause, Win...), NKRO counter, chatter detection, key history.
class KeyboardTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit KeyboardTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    // Installed on the application only while this module is active: swallows Qt
    // key events so navigation/shortcut keys don't move the module list or
    // scroll the view during testing (Raw Input still drives the visualization).
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    // Raw Input slot: updates pressed set, visited set, NKRO maximum, chatter
    // detection (re-press within kChatterMs of the release) and the history list.
    void onKeyEvent(quint32 code, quint16 vkey, bool pressed);
    // Clears all state and counters.
    void onReset();
    // Rescales key caps to the available width.
    void relayoutKeys();
    // Refreshes the tested / NKRO / chatter labels.
    void updateCounters();
    // Display name of a physical key code.
    QString keyName(quint32 code) const;
    // (Re)builds the on-screen key grid for the current layout (English ANSI or
    // Canadian-French/Apple ISO). Safe to call repeatedly — it tears down the
    // previous grid first.
    void buildKeyboardGrid();

    // UI
    QLabel *m_countLabel;
    QLabel *m_nkroLabel;
    QLabel *m_lastKeyLabel;
    QLabel *m_chatterLabel;
    QLabel *m_fnNote;
    QListWidget *m_history;
    QPushButton *m_resetBtn;
    QCheckBox *m_hideNumpad;
    QCheckBox *m_frenchLayout = nullptr; // Canadian-French (Apple) vs English
    bool m_french = false;
    QWidget *m_numpadPanel;
    QFrame *m_kbPanel = nullptr;         // container frame; grid is rebuilt inside
    QVBoxLayout *m_kbPanelLayout = nullptr;
    QWidget *m_kbHost = nullptr;         // holds the current key grid; replaced on switch
    class Card *m_lastKeyCard, *m_historyCard, *m_chatterCard;

    // key widgets
    QHash<quint32, TesterKeyLabel *> m_keyWidgets;
    QList<QPair<TesterKeyLabel *, int>> m_scalable; // widget + logical width
    QSet<quint32> m_numpadCodes;

    // state
    QSet<quint32> m_down;
    QSet<quint32> m_visited;
    QHash<quint32, qint64> m_lastRelease; // ns timestamps from m_clock
    QHash<quint32, int> m_chatterCount;
    int m_maxSimultaneous = 0;
    QElapsedTimer m_clock;

    // re-press faster than this after a release counts as key chatter
    static constexpr qint64 kChatterMs = 35;
};

#endif // KEYBOARDTESTMODULE_H
