// =============================================================================
// ScreenTestModule.h - display information and full-screen colour/gradient test.
// =============================================================================
#ifndef SCREENTESTMODULE_H
#define SCREENTESTMODULE_H

#include "TesterModule.h"
#include "common/DisplayInfoProvider.h"

#include <QDialog>

class QLabel;
class QVBoxLayout;
class QScreen;

// Fullscreen solid-color / gradient fill test. Click or arrow keys advance
// through the patterns, Esc exits. Shown on a chosen monitor.
class FillPatternDialog : public QDialog {
    Q_OBJECT
public:
    explicit FillPatternDialog(QScreen *screen, const QString &hint,
                               QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // Moves to the next/previous pattern; closes after the last.
    void advance(int step);
    // index of the current colour/gradient pattern
    int m_stage = 0;
    QString m_hint;
};

// Per-monitor info cards (precise refresh rate, HDR, EDID) + fill test.
class ScreenTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit ScreenTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;

private:
    // Rebuilds one info card per monitor from DisplayInfoProvider.
    void rebuildCards();
    // Opens the full-screen colour fill test on the chosen monitor.
    void launchFillTest(const MonitorInfo &info);

    QLabel *m_usageLabel;
    QVBoxLayout *m_cardsLayout;
    QList<QWidget *> m_cards;
    QList<MonitorInfo> m_monitors;
};

#endif // SCREENTESTMODULE_H
