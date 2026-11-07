// =============================================================================
// MainWindow.h - the QuickReports main window.
//
// Responsibilities
//   * Builds the UI: left sidebar (tools / language / version), a tab widget with
//     System Info, Storage, Motherboard/Battery, Troubleshoot and Hardware Test.
//   * Owns the HardwareModel, which runs on a worker QThread and streams scan
//     results back through signals (progressive population of the cards).
//   * Hosts the admin tools: Windows Update, time sync, Office installer, disk
//     clear/online/eject/extend/new-volume, system clone.
//   * Holds the i18n tables (en/fr/zh) and re-renders on language change.
//
// Implementation is split across three files to keep each one manageable:
//   MainWindow.cpp          - construction, UI layout, scan slots, translations
//   MainWindow_Actions.cpp  - sidebar / troubleshoot button handlers
//   MainWindow_DiskOps.cpp  - disk related operations and cloning UI
// =============================================================================
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include <QTabWidget>
#include <QComboBox>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProcess>
#include <QDialog>
#include <QTimer>
#include <QMessageBox>
#include <QThread>
#include <QMap>
#include <QMenu>
#include <QSplashScreen>
#include <QProgressDialog>
#include <QPair>
#include <QUrl>
#include <functional>

#include "HardwareModel.h"
#include "SystemInfoWidget.h"
#include "hardwaretester/HardwareTesterWidget.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // `splash` (optional) is kept so the constructor can update its message text
    // while the window is being built.
    explicit MainWindow(QSplashScreen *splash = nullptr, QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    // ---- Sidebar / toolbar actions ----------------------------------------
    void onRefreshClicked();
    void onInstallSoftwaresClicked();
    void onUpdatesClicked();
    void onUpdateCheckFinished(int exitCode, QProcess::ExitStatus status);
    void onSyncTimeClicked();
    // Troubleshoot: when the sidebar "Auto Time Zone & Sync" doesn't fix the
    // clock, this points w32time at Google's NTP server and re-enables the
    // automatic time zone before forcing a resync.
    void onFixTimeServerClicked();
    // Troubleshoot: HP Hotkey fix (brightness keys dead / popup on every key
    // press). First click installs Drivers\sp91903.exe (needs a reboot); once
    // the "HP Hotkey Service" exists, the next click disables the legacy
    // "HP Hotkey UWP Service" that causes the popups.
    void onHpHotkeyFixClicked();
    // Troubleshoot: installs Lenovo Commercial Vantage from the Microsoft Store
    // (winget, msstore source) or, if that isn't possible, opens its Store page.
    void onLenovoVantageClicked();
    bool downloadFile(const QUrl &url, const QString &destPath);
    void onCreateOfficeFolderClicked();
    void onDeviceEncryptionClicked();
    void onWordAccountClicked();
    // Switches UI language ("en"/"fr"/"zh") and re-renders cached scan data.
    void onLanguageChanged(const QString &langCode);
    void onToggleLog();
    void onStorageContextMenu(const QPoint &pos);

    // Portable-package helpers: copy the app to the desktop / delete the dist copy.
    void onCopyToDesktopClicked();
    void onDeleteDistClicked();

    // ---- Scan progress & results (connected to HardwareModel signals) ----

    void updateProgress(int current, int total, const QString &label);
    void onScanFinished(const QString &title, const QString &summary, const QList<StorageDisk> &storage, const QMap<QString, QString> &motherboard, const QList<BatteryInfo> &battery, const SystemSummaryData &overview);
    void onScanError(const QString &msg);

    // Progressive scan slots — populate the UI as each piece arrives.
    void onCardReady(const QString &stage, const SystemSummaryData &overview);
    void onStorageReady(const QList<StorageDisk> &storage);
    void onMotherboardReady(const QMap<QString, QString> &motherboard);
    void onBatteryReady(const QList<BatteryInfo> &battery);
    void detectDisplayInfo(); // reads display info on the UI thread, after render
    void onClearDisk(const QString &diskNum);
    void onSetDiskOnline(const QString &diskNum);
    void onEjectDisk(const QString &diskNum);
    // Disk insert/removal: refresh only the storage details, not every subsystem.
    void refreshStorageOnly();
    void onStorageRefreshed(const QList<StorageDisk> &storage);
    void onExtendSystemVolume();
    // Absorbs tail unallocated space into C: when a recovery partition sits
    // between C: and that space (a plain extend can't reach it). Runs the
    // reagentc/diskpart flow on a worker thread behind a progress dialog.
    void onMergeAcrossRecovery();
    void onNewVolume(const QString &diskNum);
    // Runs a blocking disk operation on a background thread behind a modal busy
    // dialog, so the UI never freezes ("not responding") and no reentrancy is
    // possible. Returns the operation's {ok, message} result.
    QPair<bool, QString> runDiskOpWithBusy(const QString &message,
                                           std::function<QPair<bool, QString>()> work);
    void onSystemCloneAuto(const QString &targetDiskNum);
    void onSystemCloneDisk(const QString &targetDiskNum, int cloneMode = 0);

private:
    // ---- UI construction ---------------------------------------------------
    void setupUi();
    void retranslateUi();
    void applyStylesheet();
    void connectSignals();
    void checkForUpdates();
    void populateStorageTree(const QList<StorageDisk> &storage);
    void populateMotherboardTree(const QMap<QString, QString> &motherboard);

    // ---- UI widgets (owned by Qt's parent/child tree) ---------------------
    QWidget *sidebar;
    QToolButton *sidebarToggleBtn = nullptr; // slim handle between sidebar and content
    bool m_sidebarUserToggled = false;       // once the user toggles manually, stop auto-collapsing
    void setSidebarCollapsed(bool collapsed);
    QLabel *osLabel;
    QPushButton *refreshBtn;
    QPushButton *officeBtn;
    QPushButton *openUpdatesBtn;
    QLabel *updateBadge; // shows pending-update count beside the Windows Update button
    QPushButton *syncTimeBtn;
    QPushButton *officeFolderBtn;
    QPushButton *deviceEncryptionBtn;
    QPushButton *wordAccountBtn; // nullptr when Microsoft Word is not installed
    QPushButton *hardwareTestBtn;
    QComboBox *langCombo;
    QLabel *versionLabel;

    QProgressBar *progressBar;
    QLabel *stepLabel;
    QTabWidget *tabs;
    SystemInfoWidget *systemInfoTab;
    QTreeWidget *storageTree;
    QTreeWidget *motherboardTree;

    // Troubleshoot tab
    QWidget *troubleshootTab = nullptr;
    QLabel *fixTimeTitle = nullptr;
    QLabel *fixTimeDesc = nullptr;
    QPushButton *fixTimeServerBtn = nullptr;
    QLabel *hpHotkeyTitle = nullptr;
    QLabel *hpHotkeyDesc = nullptr;
    QPushButton *hpHotkeyBtn = nullptr;
    QLabel *lenovoVantageTitle = nullptr;
    QLabel *lenovoVantageDesc = nullptr;
    QPushButton *lenovoVantageBtn = nullptr;
    HardwareTesterWidget *hardwareTestTab = nullptr;
    QDialog *hardwareTestDialog = nullptr;

    QPushButton *logToggleBtn;
    QPushButton *copyToDesktopBtn;
    QPushButton *deleteDistBtn;

    QPlainTextEdit *logOutput;
    QStatusBar *statusBar;

    // Mounts an .iso installer, runs its Setup.exe, and remembers the path so it
    // is dismounted on close. This is the original "run office setup" behaviour,
    // now shared by every .iso the Install Softwares picker launches.
    void installIsoImage(const QString &imagePath);
    void dismountOfficeImage();
    void closeEvent(QCloseEvent *event) override;

    // Watches for disk arrival/removal (WM_DEVICECHANGE) to auto-refresh.
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
    void registerDeviceNotifications();
    void *m_devNotify = nullptr;              // HDEVNOTIFY handle
    QTimer *m_deviceChangeTimer = nullptr;    // debounce rapid device events

    // ---- Logic / state -----------------------------------------------------
    HardwareModel *hardwareModel;
    QThread *workerThread;
    QString currentLang;
    QMap<QString, QMap<QString, QString>> translations;
    QSplashScreen *splashScreen; // Store pointer to splash
    QStringList mountedImagePaths; // every installer .iso currently mounted (dismounted on close)

    QProcess *updateCheckProc = nullptr; // background Windows Update check
    QString updateCheckScriptPath;

    // Cached last scan results, so the displayed info can be re-rendered when the
    // user switches language without re-scanning the hardware.
    bool m_hasScan = false;
    bool m_storageRefreshing = false; // guards overlapping storage-only refreshes
    QString m_scanTitle;
    QString m_scanSummary;
    QList<StorageDisk> m_scanStorage;
    QMap<QString, QString> m_scanMotherboard;
    QList<BatteryInfo> m_scanBattery;
    SystemSummaryData m_scanOverview;

    // Clone state
    QString systemDiskNum; // Stores the disk number of the system drive (C:)

    // Fills `translations` (see Translations.cpp / TesterTranslations.cpp).
    void loadTranslations();
    // Looks `key` up in the current language, falling back to English, then the key.
    QString getTranslation(const QString &key);
    // Appends a timestamped line to the collapsible log panel.
    void appendLog(const QString &msg);
};

#endif // MAINWINDOW_H
