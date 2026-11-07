// =============================================================================
// NewVolumeWizard.cpp - implementation of the New Volume wizard pages.
// =============================================================================
#include "NewVolumeWizard.h"
#include <QLabel>
#include <QRadioButton>
#include <QSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QButtonGroup>
#include <QFileDialog>
#include <QMessageBox>
#include <QStorageInfo>
#include <QDir>
#include <QStandardItemModel>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// Human-readable size with binary units (B..TiB).
QString bytesText(qint64 bytes) {
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u == 0 ? 0 : 2).arg(units[u]);
}

// A wizard page whose Next/Finish is gated by a validator and that can refresh
// itself whenever it is shown.
// (see comment above)
class Page : public QWizardPage {
public:
    std::function<bool()> validate;
    std::function<void()> onShow;
    bool validatePage() override { return validate ? validate() : true; }
    void initializePage() override { if (onShow) onShow(); }
};

} // namespace

// Sets window title/buttons (custom button = "use Disk Management" fallback)
// and adds the five pages in order.
NewVolumeWizard::NewVolumeWizard(const QString &diskNum, const QString &model, bool needsInit,
                                 qint64 largestFreeBytes, Translator tr, QWidget *parent)
    : QWizard(parent), m_diskNum(diskNum), m_model(model), m_needsInit(needsInit),
      m_freeBytes(largestFreeBytes), m_maxMB(largestFreeBytes / (1024 * 1024)), m_tr(std::move(tr)) {
    setWindowTitle(m_tr("nvw_title"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::HaveCustomButton1, true);
    setButtonText(QWizard::NextButton, m_tr("nvw_next"));
    setButtonText(QWizard::BackButton, m_tr("nvw_back"));
    setButtonText(QWizard::FinishButton, m_tr("nvw_finish"));
    setButtonText(QWizard::CancelButton, m_tr("nvw_cancel"));
    setButtonText(QWizard::CustomButton1, m_tr("nvw_fallback"));
    setMinimumSize(560, 420);

    // Fallback: hand over to Windows' own Disk Management.
    connect(this, &QWizard::customButtonClicked, this, [this](int which) {
        if (which == QWizard::CustomButton1) {
            m_fallback = true;
            reject();
        }
    });

    addPage(makeIntroPage());
    addPage(makeSizePage());
    addPage(makeAssignPage());
    addPage(makeFormatPage());
    addPage(makeSummaryPage());
}

// Page 1: welcome text; for an uninitialized disk also GPT/MBR style choice.
QWizardPage *NewVolumeWizard::makeIntroPage() {
    auto *p = new Page;
    p->setTitle(m_tr("nvw_intro_title"));
    auto *lay = new QVBoxLayout(p);

    auto *text = new QLabel(m_tr("nvw_intro_text")
                                .replace("{disk_num}", m_diskNum)
                                .replace("{model}", m_model)
                                .replace("{free}", bytesText(m_freeBytes)), p);
    text->setWordWrap(true);
    lay->addWidget(text);

    if (m_needsInit) {
        auto *warn = new QLabel(m_tr("nvw_intro_init"), p);
        warn->setWordWrap(true);
        lay->addSpacing(8);
        lay->addWidget(warn);
        m_gpt = new QRadioButton(m_tr("nvw_style_gpt"), p);
        m_mbr = new QRadioButton(m_tr("nvw_style_mbr"), p);
        m_gpt->setChecked(true);
        lay->addWidget(m_gpt);
        lay->addWidget(m_mbr);
        auto *note = new QLabel(m_tr("nvw_intro_warn_init"), p);
        note->setWordWrap(true);
        note->setStyleSheet("color:#b42318;");
        lay->addWidget(note);
    }
    lay->addStretch();
    return p;
}

// Page 2: volume size in MiB (8 .. largest free extent) with a "use max" button.
QWizardPage *NewVolumeWizard::makeSizePage() {
    auto *p = new Page;
    p->setTitle(m_tr("nvw_size_title"));
    p->setSubTitle(m_tr("nvw_size_sub"));
    auto *lay = new QVBoxLayout(p);

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(m_tr("nvw_size_field"), p));
    m_sizeSpin = new QSpinBox(p);
    m_sizeSpin->setRange(8, int(qMin<qint64>(m_maxMB, 2000000000)));
    m_sizeSpin->setValue(m_sizeSpin->maximum());
    m_sizeSpin->setGroupSeparatorShown(true);
    row->addWidget(m_sizeSpin);
    auto *maxBtn = new QPushButton(m_tr("nvw_size_use_max"), p);
    connect(maxBtn, &QPushButton::clicked, m_sizeSpin, [this] { m_sizeSpin->setValue(m_sizeSpin->maximum()); });
    row->addWidget(maxBtn);
    row->addStretch();
    lay->addLayout(row);
    lay->addWidget(new QLabel(m_tr("nvw_size_max").replace("{max}", QLocale().toString(m_maxMB)), p));
    lay->addStretch();
    return p;
}

// Page 3: mount choice - auto letter / specific free letter (D..Z) / empty NTFS
// folder / none. validate() checks the folder is an empty NTFS directory.
QWizardPage *NewVolumeWizard::makeAssignPage() {
    auto *p = new Page;
    p->setTitle(m_tr("nvw_assign_title"));
    p->setSubTitle(m_tr("nvw_assign_sub"));
    auto *lay = new QVBoxLayout(p);

    m_mountAuto = new QRadioButton(m_tr("nvw_assign_auto"), p);
    m_mountLetter = new QRadioButton(m_tr("nvw_assign_letter"), p);
    m_mountFolder = new QRadioButton(m_tr("nvw_assign_folder"), p);
    m_mountNone = new QRadioButton(m_tr("nvw_assign_none"), p);
    m_mountAuto->setChecked(true);

    m_letterCombo = new QComboBox(p);
#ifdef Q_OS_WIN
    const DWORD used = GetLogicalDrives();
#else
    const quint32 used = 0;
#endif
    for (char c = 'D'; c <= 'Z'; ++c)
        if (!(used & (1u << (c - 'A'))))
            m_letterCombo->addItem(QStringLiteral("%1:").arg(c), QString(QChar(c)));
    m_letterCombo->setEnabled(false);

    m_folderEdit = new QLineEdit(p);
    m_folderEdit->setEnabled(false);
    auto *browse = new QPushButton(m_tr("nvw_browse"), p);
    browse->setEnabled(false);
    connect(browse, &QPushButton::clicked, p, [this, p] {
        const QString dir = QFileDialog::getExistingDirectory(p, m_tr("nvw_assign_folder"));
        if (!dir.isEmpty())
            m_folderEdit->setText(QDir::toNativeSeparators(dir));
    });

    auto sync = [=] {
        m_letterCombo->setEnabled(m_mountLetter->isChecked());
        m_folderEdit->setEnabled(m_mountFolder->isChecked());
        browse->setEnabled(m_mountFolder->isChecked());
    };
    for (auto *r : {m_mountAuto, m_mountLetter, m_mountFolder, m_mountNone})
        connect(r, &QRadioButton::toggled, p, sync);

    lay->addWidget(m_mountAuto);
    auto *letterRow = new QHBoxLayout;
    letterRow->addWidget(m_mountLetter);
    letterRow->addWidget(m_letterCombo);
    letterRow->addStretch();
    lay->addLayout(letterRow);
    lay->addWidget(m_mountFolder);
    auto *folderRow = new QHBoxLayout;
    folderRow->addSpacing(20);
    folderRow->addWidget(m_folderEdit);
    folderRow->addWidget(browse);
    lay->addLayout(folderRow);
    lay->addWidget(m_mountNone);
    lay->addStretch();

    p->validate = [this, p] {
        if (m_mountLetter->isChecked() && m_letterCombo->count() == 0) {
            QMessageBox::warning(p, m_tr("nvw_title"), m_tr("nvw_no_letters"));
            return false;
        }
        if (m_mountFolder->isChecked()) {
            const QString dir = m_folderEdit->text().trimmed();
            const QFileInfo fi(dir);
            const bool ok = !dir.isEmpty() && fi.isDir()
                && QDir(dir).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)
                && QStorageInfo(dir).fileSystemType().compare("ntfs", Qt::CaseInsensitive) == 0;
            if (!ok) {
                QMessageBox::warning(p, m_tr("nvw_title"), m_tr("nvw_folder_invalid"));
                return false;
            }
        }
        return true;
    };
    return p;
}

// Page 4: file system (NTFS/exFAT/FAT32), allocation unit, label, quick format.
// FAT32 is disabled for volumes over 32 GB (Windows formatter limit).
QWizardPage *NewVolumeWizard::makeFormatPage() {
    auto *p = new Page;
    p->setTitle(m_tr("nvw_format_title"));
    p->setSubTitle(m_tr("nvw_format_sub"));
    auto *form = new QFormLayout(p);

    m_fsCombo = new QComboBox(p);
    m_fsCombo->addItem("NTFS", "ntfs");
    m_fsCombo->addItem("exFAT", "exfat");
    m_fsCombo->addItem("FAT32", "fat32");
    form->addRow(m_tr("nvw_fs"), m_fsCombo);

    m_unitCombo = new QComboBox(p);
    m_unitCombo->addItem(m_tr("nvw_unit_default"), 0);
    for (int u : {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536})
        m_unitCombo->addItem(bytesText(u), u);
    form->addRow(m_tr("nvw_unit"), m_unitCombo);

    m_labelEdit = new QLineEdit(QStringLiteral("Data"), p);
    m_labelEdit->setMaxLength(32);
    form->addRow(m_tr("nvw_label"), m_labelEdit);

    m_quick = new QCheckBox(m_tr("nvw_quick"), p);
    m_quick->setChecked(true);
    form->addRow(QString(), m_quick);

    m_fatNote = new QLabel(m_tr("nvw_fat32_note"), p);
    m_fatNote->setWordWrap(true);
    m_fatNote->setStyleSheet("color:#b42318;");
    m_fatNote->hide();
    form->addRow(m_fatNote);

    // FAT32 is capped at 32 GB by Windows' formatter: disable it for bigger volumes.
    p->onShow = [this] {
        const bool tooBig = chosenSizeMB() > 32 * 1024;
        if (auto *model = qobject_cast<QStandardItemModel *>(m_fsCombo->model())) {
            if (auto *item = model->item(2))
                item->setEnabled(!tooBig);
        }
        if (tooBig && m_fsCombo->currentIndex() == 2)
            m_fsCombo->setCurrentIndex(0);
        m_fatNote->setVisible(tooBig);
    };
    return p;
}

// Page 5: read-only summary of every choice before Finish.
QWizardPage *NewVolumeWizard::makeSummaryPage() {
    auto *p = new Page;
    p->setTitle(m_tr("nvw_summary_title"));
    p->setSubTitle(m_tr("nvw_summary_sub"));
    auto *lay = new QVBoxLayout(p);
    m_summary = new QLabel(p);
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::PlainText);
    lay->addWidget(m_summary);
    lay->addStretch();
    p->setFinalPage(true);

    p->onShow = [this] {
        const NewVolumeOptions o = options();
        QStringList lines;
        lines << QStringLiteral("%1: %2 (%3)").arg(m_tr("nvw_sum_disk"), m_diskNum, m_model);
        if (o.initializeDisk)
            lines << QStringLiteral("%1: %2").arg(m_tr("nvw_sum_style"), o.partitionStyle.toUpper());
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_size_field"),
                     o.sizeMB > 0 ? QLocale().toString(o.sizeMB) : QLocale().toString(m_maxMB));
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_sum_mount"), mountSummary());
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_fs"), m_fsCombo->currentText());
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_unit"), m_unitCombo->currentText());
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_label"), o.label);
        lines << QStringLiteral("%1: %2").arg(m_tr("nvw_quick"),
                     m_tr(o.quickFormat ? "nvw_yes" : "nvw_no"));
        m_summary->setText(lines.join('\n'));
    };
    return p;
}

// (see header)
qint64 NewVolumeWizard::chosenSizeMB() const {
    return m_sizeSpin ? m_sizeSpin->value() : m_maxMB;
}

// (see header)
QString NewVolumeWizard::mountSummary() const {
    if (m_mountLetter->isChecked()) return m_letterCombo->currentText();
    if (m_mountFolder->isChecked()) return m_folderEdit->text().trimmed();
    if (m_mountNone->isChecked()) return m_tr("nvw_assign_none");
    return m_tr("nvw_assign_auto");
}

// Converts the widgets' state into a NewVolumeOptions for HardwareModel::createVolume().
NewVolumeOptions NewVolumeWizard::options() const {
    NewVolumeOptions o;
    o.diskNum = m_diskNum;
    o.initializeDisk = m_needsInit;
    if (m_needsInit && m_mbr && m_mbr->isChecked())
        o.partitionStyle = QStringLiteral("mbr");
    // Choosing (nearly) the whole extent means "no size=": diskpart then uses
    // exactly what is really free, absorbing partition-table overhead.
    const qint64 mb = chosenSizeMB();
    o.sizeMB = (mb >= m_maxMB - 32) ? 0 : mb;
    o.fileSystem = m_fsCombo->currentData().toString();
    o.unitBytes = m_unitCombo->currentData().toInt();
    o.quickFormat = m_quick->isChecked();
    o.label = m_labelEdit->text().trimmed();
    if (m_mountLetter->isChecked()) {
        o.mount = NewVolumeOptions::Letter;
        o.driveLetter = m_letterCombo->currentData().toString();
    } else if (m_mountFolder->isChecked()) {
        o.mount = NewVolumeOptions::Folder;
        o.mountFolder = m_folderEdit->text().trimmed();
    } else if (m_mountNone->isChecked()) {
        o.mount = NewVolumeOptions::NoLetter;
    }
    return o;
}
