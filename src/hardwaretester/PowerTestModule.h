// =============================================================================
// PowerTestModule.h - charger / battery live power test.
// =============================================================================
#ifndef POWERTESTMODULE_H
#define POWERTESTMODULE_H

#include "TesterModule.h"

class QLabel;
class QPushButton;
class QTimer;
class Card;

// Battery & charging test.
//
// This module intentionally does NOT grade battery health (design capacity,
// wear and cycle count already appear on the System Summary page). Its job is
// to tell, live, whether the machine is on AC (charger) or on the battery, and
// to show the instantaneous voltage / current / wattage — the readings that
// make it possible to test a battery, a charging port, or a charger by plugging
// and unplugging and watching the numbers react.
class PowerTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit PowerTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    void onTick();                 // 1 Hz refresh of source + V/A/W

    // UI
    Card *m_infoCard = nullptr;
    Card *m_sourceCard = nullptr;
    Card *m_liveCard = nullptr;
    QLabel *m_intro = nullptr;
    QLabel *m_sourceLabel = nullptr;   // big "AC" / "Battery" line
    QLabel *m_stateLabel = nullptr;    // charging / discharging / level
    QLabel *m_drawVal = nullptr;       // headline: whole-system live power draw
    QLabel *m_drawCap = nullptr;       // caption for the headline
    QLabel *m_drawExplain = nullptr;   // what the headline number means right now
    QLabel *m_voltageVal = nullptr;
    QLabel *m_currentVal = nullptr;
    QLabel *m_powerVal = nullptr;
    QLabel *m_voltageCap = nullptr;    // caption "Voltage"
    QLabel *m_currentCap = nullptr;
    QLabel *m_powerCap = nullptr;
    QLabel *m_flowLabel = nullptr;     // "charger delivering N W", etc.
    QLabel *m_note = nullptr;

    QTimer *m_timer = nullptr;
};

#endif // POWERTESTMODULE_H
