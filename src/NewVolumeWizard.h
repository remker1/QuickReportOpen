// =============================================================================
// NewVolumeWizard.h - wizard for creating a new simple volume on a disk.
// =============================================================================
#ifndef NEWVOLUMEWIZARD_H
#define NEWVOLUMEWIZARD_H

#include <QWizard>
#include <functional>
#include "HardwareModel.h"

class QLabel;
class QRadioButton;
class QSpinBox;
class QComboBox;
class QLineEdit;
class QCheckBox;
class QPushButton;

// A wizard modelled on Windows' "New Simple Volume Wizard": welcome (with
// partition style for an uninitialized disk), size, drive letter / mount
// folder, format options, and a summary. It only collects choices; the caller
// runs HardwareModel::createVolume(options()).
// (see description above)
class NewVolumeWizard : public QWizard {
    Q_OBJECT
public:
    using Translator = std::function<QString(const QString &)>;

    // largestFreeBytes: the biggest single free extent on the disk (a new
    // partition can never span two gaps). needsInit: the disk has no partitions.
    NewVolumeWizard(const QString &diskNum, const QString &model, bool needsInit,
                    qint64 largestFreeBytes, Translator tr, QWidget *parent = nullptr);

    NewVolumeOptions options() const;
    // True when the user chose the "use Windows Disk Management" fallback.
    bool fallbackRequested() const { return m_fallback; }

private:
    // One factory per wizard page, in display order:
    QWizardPage *makeIntroPage();
    QWizardPage *makeSizePage();
    QWizardPage *makeAssignPage();
    QWizardPage *makeFormatPage();
    QWizardPage *makeSummaryPage();
    // Text for the summary page describing how the volume will be mounted.
    QString mountSummary() const;
    // Size currently chosen on the size page, in MiB.
    qint64 chosenSizeMB() const;

    // State captured from the constructor arguments:
    QString m_diskNum, m_model;
    bool m_needsInit;
    qint64 m_freeBytes;
    qint64 m_maxMB;
    Translator m_tr;
    bool m_fallback = false;

    // Widgets that hold the user's choices (read back by options()):
    QRadioButton *m_gpt = nullptr, *m_mbr = nullptr;
    QSpinBox *m_sizeSpin = nullptr;
    QRadioButton *m_mountAuto = nullptr, *m_mountLetter = nullptr,
                 *m_mountFolder = nullptr, *m_mountNone = nullptr;
    QComboBox *m_letterCombo = nullptr;
    QLineEdit *m_folderEdit = nullptr;
    QComboBox *m_fsCombo = nullptr, *m_unitCombo = nullptr;
    QLineEdit *m_labelEdit = nullptr;
    QCheckBox *m_quick = nullptr;
    QLabel *m_fatNote = nullptr;
    QLabel *m_summary = nullptr;
};

#endif // NEWVOLUMEWIZARD_H
