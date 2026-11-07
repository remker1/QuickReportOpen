// =============================================================================
// HardwareTesterWidget.h - container of the interactive hardware tests.
//
// Left: navigation list (keyboard, trackpad, touch, camera, audio, screen, power,
// fingerprint). Right: stacked module pages.
// =============================================================================
#ifndef HARDWARETESTERWIDGET_H
#define HARDWARETESTERWIDGET_H

#include <QWidget>
#include <QList>
#include <QPair>
#include <QString>
#include <functional>

class QListWidget;
class QStackedWidget;
class TesterModule;
class QShowEvent;
class QHideEvent;

// The deep hardware tester page: left module navigation + stacked module
// pages. Drop-in replacement for the old HardwareTestWidget tab — keeps the
// retranslate() / testResults() contract consumed by MainWindow.
// (see description above)
class HardwareTesterWidget : public QWidget {
    Q_OBJECT
public:
    explicit HardwareTesterWidget(std::function<QString(const QString&)> translator,
                                  QWidget *parent = nullptr);

    void retranslate();

    // Aggregated results of every module, in module order.
    QList<QPair<QString, QString>> testResults() const;

private:
    // Appends a nav entry (translation key) and its page to the list and stack.
    void addModule(const QString &navKey, TesterModule *module);
    // Switches page: leaves the old module, enters the new one (if the tester is visible).
    void onNavChanged(int row);
    // Tester window shown: activates the current module.
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

    std::function<QString(const QString&)> tr_;
    QListWidget *m_nav;
    QStackedWidget *m_stack;

    // One registered module and the translation key of its nav label.
    struct Entry { QString navKey; TesterModule *module; };
    QList<Entry> m_entries;
    int m_currentRow = -1;
    // true while the tester is visible (modules only run then)
    bool m_active = false;
};

#endif // HARDWARETESTERWIDGET_H
