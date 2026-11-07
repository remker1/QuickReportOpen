// =============================================================================
// TesterModule.h - abstract base class of every hardware-tester page.
//
// A module is a QWidget shown in the tester's stacked view. The host
// (HardwareTesterWidget) calls moduleEntered()/moduleLeft() when the page becomes
// visible/hidden so modules only hold devices while they are on screen.
// =============================================================================
#ifndef TESTERMODULE_H
#define TESTERMODULE_H

#include <QWidget>
#include <QList>
#include <QPair>
#include <QString>
#include <functional>

// Base class for every page of the hardware tester. Modules acquire expensive
// resources (Raw Input registration, camera, PDH queries) in moduleEntered()
// and must release them in moduleLeft() so an idle tester costs nothing.
// (see description above)
class TesterModule : public QWidget {
    Q_OBJECT
public:
    // Maps a translation key to text in the current UI language.
    using Translator = std::function<QString(const QString&)>;

    explicit TesterModule(Translator translator, QWidget *parent = nullptr)
        : QWidget(parent), tr_(std::move(translator)) {}

    // Re-labels all visible text after a language switch.
    // (pure virtual: every module must implement it)
    virtual void retranslate() = 0;

    // Called when the module page becomes/stops being the visible page.
    virtual void moduleEntered() {}
    virtual void moduleLeft() {}

    // Results this module wants in the PDF report (translated name -> summary),
    // in first-recorded order. Mirrors the old HardwareTestWidget contract.
    // (see comment above)
    QList<QPair<QString, QString>> results() const { return m_results; }

protected:
    // Records/overwrites one result line by translation key.
    void recordResult(const QString &nameKey, const QString &value) {
        const QString name = tr_(nameKey);
        for (auto &r : m_results) {
            if (r.first == name) { r.second = value; return; }
        }
        m_results.append({name, value});
    }

    // translator supplied by the host widget
    Translator tr_;

private:
    // insertion-ordered (translated name, value) pairs
    QList<QPair<QString, QString>> m_results;
};

#endif // TESTERMODULE_H
