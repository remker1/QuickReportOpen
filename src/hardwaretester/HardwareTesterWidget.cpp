// =============================================================================
// HardwareTesterWidget.cpp - module registration and page switching.
// =============================================================================
#include "HardwareTesterWidget.h"
#include "AudioTestModule.h"
#include "CameraTestModule.h"
#include "PowerTestModule.h"
#include "FingerprintTestModule.h"
#include "KeyboardTestModule.h"
#include "TrackpadTestModule.h"
#include "TouchTestModule.h"
#include "ScreenTestModule.h"
#include "TesterModule.h"

#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QShowEvent>
#include <QStackedWidget>
#include <QVBoxLayout>

// Builds the layout (nav list left, stacked pages right) and registers the
// modules. Registration order = navigation order = report order.
HardwareTesterWidget::HardwareTesterWidget(std::function<QString(const QString&)> translator,
                                           QWidget *parent)
    : QWidget(parent), tr_(std::move(translator)) {
    setObjectName("HardwareTester");
    // The tester is hosted in a separate dialog. Keep text readable on its
    // white/transparent buttons regardless of the main-window button palette.
    setStyleSheet(
        "QPushButton { color: #000000; }"
        "QPushButton:disabled { color: #555555; }");
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_nav = new QListWidget(this);
    m_nav->setObjectName("TesterNav");
    m_nav->setFixedWidth(200);
    m_nav->setFrameShape(QFrame::NoFrame);
    lay->addWidget(m_nav);

    m_stack = new QStackedWidget(this);
    m_stack->setObjectName("TesterStack");
    lay->addWidget(m_stack, 1);

    // Module order defines nav order and PDF report order. Remaining
    // placeholders are replaced as modules land.
    addModule("ht_nav_keyboard", new KeyboardTestModule(tr_, this));
    addModule("ht_nav_trackpad", new TrackpadTestModule(tr_, this));
    addModule("ht_nav_touch", new TouchTestModule(tr_, this));
    addModule("ht_nav_camera", new CameraTestModule(tr_, this));
    addModule("ht_nav_audio", new AudioTestModule(tr_, this));
    addModule("ht_nav_screen", new ScreenTestModule(tr_, this));
    addModule("ht_nav_power", new PowerTestModule(tr_, this));
    addModule("ht_nav_fingerprint", new FingerprintTestModule(tr_, this));

    connect(m_nav, &QListWidget::currentRowChanged, this,
            &HardwareTesterWidget::onNavChanged);
    m_nav->setCurrentRow(0);
}

// Adds a navigation row and the module page.
void HardwareTesterWidget::addModule(const QString &navKey, TesterModule *module) {
    m_nav->addItem(tr_(navKey));
    m_stack->addWidget(module);
    m_entries.append({navKey, module});
}

// Nav selection changed: moduleLeft() on the old page, moduleEntered() on the new
// one, but only while the tester is visible (m_active).
void HardwareTesterWidget::onNavChanged(int row) {
    if (row == m_currentRow)
        return;
    if (m_active && m_currentRow >= 0 && m_currentRow < m_entries.size())
        m_entries[m_currentRow].module->moduleLeft();
    m_currentRow = row;
    if (row >= 0 && row < m_entries.size()) {
        m_stack->setCurrentIndex(row);
        if (m_active)
            m_entries[row].module->moduleEntered();
    }
}

// Tester shown -> start the current module.
void HardwareTesterWidget::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (!m_active) {
        m_active = true;
        if (m_currentRow >= 0 && m_currentRow < m_entries.size())
            m_entries[m_currentRow].module->moduleEntered();
    }
}

// Tester hidden -> stop the current module so devices are released.
void HardwareTesterWidget::hideEvent(QHideEvent *event) {
    if (m_active) {
        if (m_currentRow >= 0 && m_currentRow < m_entries.size())
            m_entries[m_currentRow].module->moduleLeft();
        m_active = false;
    }
    QWidget::hideEvent(event);
}

// Re-applies translations to the nav labels and every module.
void HardwareTesterWidget::retranslate() {
    for (int i = 0; i < m_entries.size(); ++i) {
        m_nav->item(i)->setText(tr_(m_entries[i].navKey));
        m_entries[i].module->retranslate();
    }
}

// Concatenates the results of all modules (for the inspection report).
QList<QPair<QString, QString>> HardwareTesterWidget::testResults() const {
    QList<QPair<QString, QString>> all;
    for (const Entry &e : m_entries)
        all += e.module->results();
    return all;
}
