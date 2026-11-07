// =============================================================================
// SystemInfoWidget.h - the "System Summary" tab (hardware cards).
// =============================================================================
#ifndef SYSTEMINFOWIDGET_H
#define SYSTEMINFOWIDGET_H

#include <QScrollArea>
#include "HardwareModel.h"

class QGridLayout;
class QFrame;

// Scrollable page of "cards" (System, CPU, GPU, Memory, Display, Network,
// Activation, Storage, Battery). Can be rebuilt in one go (updateData) or filled
// progressively while the scan runs (beginProgressive + add*Card).
class SystemInfoWidget : public QScrollArea {
    Q_OBJECT
public:
    explicit SystemInfoWidget(QWidget *parent = nullptr);

    // Rebuilds the whole page from complete data. `lang` (en/zh/fr) selects the label
    // language; also used to re-render after a language switch.
    void updateData(const SystemSummaryData &overview,
                    const QList<StorageDisk> &storage,
                    const QList<BatteryInfo> &battery,
                    const QMap<QString, QString> &motherboard,
                    const QString &lang = "en");

    // Shows a centered placeholder message while the scan is running.
    // (see comment above)
    void showLoading(const QString &message);

    // Progressive (streamed) rendering: begin, then add cards as data arrives.
    void beginProgressive(const QString &lang);
    void addCard(const QString &stage, const SystemSummaryData &overview);
    void addStorageCard(const QList<StorageDisk> &storage);
    void addBatteryCards(const QList<BatteryInfo> &battery);

private:
    // Adds a card to the 2-column grid; full-width cards take a whole row.
    void placeCard(QFrame *card, bool fullWidth);

    // grid used by progressive rendering (null when not in progressive mode)
    QGridLayout *m_grid = nullptr;
    // next free cell: row / column
    int m_gridRow = 0;
    int m_gridCol = 0;
};

#endif // SYSTEMINFOWIDGET_H
