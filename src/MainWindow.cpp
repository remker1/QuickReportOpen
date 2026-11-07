// =============================================================================
// MainWindow.cpp - construction, UI layout, scan result handling, translations
// and styling of the main window. See MainWindow.h for the overall design; the
// sidebar/troubleshoot actions are in MainWindow_Actions.cpp and the disk
// operations in MainWindow_DiskOps.cpp.
// =============================================================================
#include "MainWindow.h"
#include <QHeaderView>
#include <QDir>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QDebug>
#include <QFile>
#include <QStandardPaths>
#include <QApplication>
#include <QProgressDialog>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QSettings>
#include <QTextStream>
#include <QRegularExpression>
#include <QLocale>
#include <QElapsedTimer>
#include <QDateTime>
#include <QPointer>
#include <QInputDialog>
#include <QStorageInfo>
#include <QSet>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QEventLoop>
#include <QScrollBar>
#include <memory>
#include <algorithm>
#include <thread>
#include "CrashHandler.h"
#include "CloneProgressDialog.h"
#include "Translations.h"
#include "hardwaretester/TesterTranslations.h"


#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
// GUID_DEVINTERFACE_DISK — physical disk device interface class.
// Device-interface class GUID used to be told when physical disks arrive/leave.
static const GUID QR_GUID_DEVINTERFACE_DISK =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };
#endif

// Pick the initial UI language from the OS locale (falls back to English).
static QString detectSystemLanguage() {
    switch (QLocale::system().language()) {
        case QLocale::Chinese: return "zh";
        case QLocale::French:  return "fr";
        default:               return "en";
    }
}

#ifdef Q_OS_WIN
// Returns WINWORD.EXE's full path if Microsoft Word is installed, else empty.
// Primary source is the App Paths registry key; Click-to-Run installs don't
// always populate it, so we also scan the standard install directories.
// True if the system has a touch-capable digitizer.
static bool hasTouchScreen() {
    int digitizer = GetSystemMetrics(SM_DIGITIZER);
    bool ready = (digitizer & NID_READY) != 0;
    bool touch = (digitizer & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) != 0;
    if (ready && touch)
        return true;
    return GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
}
#endif

// Constructor: loads translations, builds the UI, wires signals, applies the
// language, performs best-effort start-up housekeeping (Fast Startup off,
// interrupted-clone recovery), arms disk hot-plug detection and starts the first scan.
MainWindow::MainWindow(QSplashScreen *splash, QWidget *parent)
    : QMainWindow(parent), hardwareModel(nullptr), workerThread(nullptr), splashScreen(splash)
{
    loadTranslations();
    currentLang = detectSystemLanguage(); // follow OS language until user changes it
    setupUi();
    applyStylesheet();
    connectSignals();
    retranslateUi();

    // Silently turn off Fast Startup on launch (best effort; persists only when
    // the app runs with administrator rights). No detection/prompt.
    #ifdef Q_OS_WIN
    HardwareModel::disableFastStartup();

    // A shrink-to-fit clone that was killed midway leaves the source C: shrunk;
    // grow it back (and re-enable hibernation if we turned it off).
    std::thread([this]() {
        const QString msg = HardwareModel::recoverInterruptedClone();
        if (!msg.isEmpty())
            QMetaObject::invokeMethod(this, [this, msg]() { appendLog(msg); }, Qt::QueuedConnection);
    }).detach();
    #endif

    // Debounced auto-refresh when a disk is inserted/removed. A single plug event
    // fires several WM_DEVICECHANGE messages, so we coalesce them into one refresh.
    m_deviceChangeTimer = new QTimer(this);
    m_deviceChangeTimer->setSingleShot(true);
    m_deviceChangeTimer->setInterval(1500);
    connect(m_deviceChangeTimer, &QTimer::timeout, this, [this]() {
        appendLog(getTranslation("disk_change_detected"));
        refreshStorageOnly(); // disk change → refresh only the storage details
    });
    #ifdef Q_OS_WIN
    registerDeviceNotifications();
    #endif

    // Initial refresh (also triggers the Windows Update badge check).
    onRefreshClicked();
}

// Destructor: unregisters device notifications and shuts the worker thread down
// before deleting the model (the model lives on that thread).
MainWindow::~MainWindow() {
    #ifdef Q_OS_WIN
    if (m_devNotify) {
        UnregisterDeviceNotification(static_cast<HDEVNOTIFY>(m_devNotify));
        m_devNotify = nullptr;
    }
    #endif

    // Clean up thread and model safely
    if (workerThread) {
        workerThread->quit();
        workerThread->wait();
        delete workerThread;
        workerThread = nullptr;
    }
    if (hardwareModel) {
        delete hardwareModel;
        hardwareModel = nullptr;
    }
}

#ifdef Q_OS_WIN
// Asks Windows to post WM_DEVICECHANGE for disk interface arrival/removal.
void MainWindow::registerDeviceNotifications() {
    DEV_BROADCAST_DEVICEINTERFACE filter;
    ZeroMemory(&filter, sizeof(filter));
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = QR_GUID_DEVINTERFACE_DISK;

    m_devNotify = RegisterDeviceNotification(
        reinterpret_cast<HWND>(winId()), &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
}

// Raw Windows message hook: turns disk/volume arrival & removal into a debounced
// storage refresh (the timer coalesces the burst of messages one plug produces).
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == WM_DEVICECHANGE &&
            (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE)) {
            auto *hdr = reinterpret_cast<DEV_BROADCAST_HDR *>(msg->lParam);
            // React to physical disks and volumes (drive-letter) arriving/leaving.
            if (hdr && (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE ||
                        hdr->dbch_devicetype == DBT_DEVTYP_VOLUME)) {
                if (m_deviceChangeTimer)
                    m_deviceChangeTimer->start(); // debounce; restarts on each event
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#else
void MainWindow::registerDeviceNotifications() {}
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

// Builds the whole widget tree: sidebar (scrollable button column, language
// combo, version), collapse handle, progress bar, tabs (summary, storage tree,
// motherboard tree, troubleshoot), hardware-tester dialog and the log panel.
// All visible text is placeholder English here; retranslateUi() sets real text.
void MainWindow::setupUi() {
    // Set the window icon from resources
    setWindowIcon(QIcon(":/Logo1.png"));
    resize(1000, 700);

    QWidget *central = new QWidget(this);
    central->setObjectName("CentralWidget");
    QHBoxLayout *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // --- Sidebar ---
    sidebar = new QWidget(this);
    sidebar->setObjectName("Sidebar");
    sidebar->setFixedWidth(260);
    QVBoxLayout *sidebarOuter = new QVBoxLayout(sidebar);
    sidebarOuter->setContentsMargins(20, 26, 20, 18);
    sidebarOuter->setSpacing(10);

    osLabel = new QLabel(this);
    osLabel->setWordWrap(true);
    QFont font;
    font.setPointSize(10);
    font.setBold(true);
    osLabel->setFont(font);
    osLabel->setStyleSheet("color: #5C7C98; font-size: 14px;");
    sidebarOuter->addWidget(osLabel);

    // Separator
    QFrame *line = new QFrame();
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    line->setStyleSheet("background-color: #E0E0E0; color: #E0E0E0;");
    sidebarOuter->addWidget(line);

    // Buttons go inside a scroll area so the sidebar never overflows vertically
    // no matter how many buttons there are.
    QScrollArea *sidebarScroll = new QScrollArea(sidebar);
    sidebarScroll->setWidgetResizable(true);
    sidebarScroll->setFrameShape(QFrame::NoFrame);
    sidebarScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Scope every transparency rule to a specific object so it never clears the
    // child buttons' backgrounds.
    sidebarScroll->viewport()->setObjectName("SbViewport");
    sidebarScroll->setStyleSheet(
        "QScrollArea{background:transparent; border:none;}"
        " QWidget#SbViewport{background:transparent;}");
    QWidget *btnHost = new QWidget;
    btnHost->setObjectName("BtnHost");
    // Scoped selector so it doesn't clear the child buttons' backgrounds.
    btnHost->setStyleSheet("QWidget#BtnHost{background:transparent;}");
    QVBoxLayout *sidebarLayout = new QVBoxLayout(btnHost);
    sidebarLayout->setContentsMargins(0, 2, 8, 2);
    sidebarLayout->setSpacing(9);

    refreshBtn = new QPushButton("Refresh Hardware Info", this);
    sidebarLayout->addWidget(refreshBtn);

    officeBtn = new QPushButton("Install Softwares", this);
    sidebarLayout->addWidget(officeBtn);


    // Windows Update button with a notification badge that appears when updates
    // (including driver updates) are pending.
    openUpdatesBtn = new QPushButton("Open Windows Update", this);
    QWidget *updatesRow = new QWidget(this);
    QHBoxLayout *updatesRowLayout = new QHBoxLayout(updatesRow);
    updatesRowLayout->setContentsMargins(0, 0, 0, 0);
    updatesRowLayout->setSpacing(5);
    updatesRowLayout->addWidget(openUpdatesBtn, 1);

    updateBadge = new QLabel(updatesRow);
    updateBadge->setAlignment(Qt::AlignCenter);
    updateBadge->setMinimumSize(20, 20);
    updateBadge->setStyleSheet(
        "background:#C0392B; color:white; border-radius:10px;"
        " padding:0 6px; font-weight:bold; font-size:11px;");
    updateBadge->hide();
    updatesRowLayout->addWidget(updateBadge, 0);

    sidebarLayout->addWidget(updatesRow);

    syncTimeBtn = new QPushButton("Auto Time Zone && Sync", this);
    sidebarLayout->addWidget(syncTimeBtn);

    officeFolderBtn = new QPushButton("Create Office Folder", this);
    sidebarLayout->addWidget(officeFolderBtn);

    // Always available: Word may be installed after launch (e.g. via "Install
    // Softwares"), so the button must not be gated on startup detection. When
    // Word is absent the click handler shows a friendly "not found" message.
    wordAccountBtn = new QPushButton("Open Word Account", this);
    sidebarLayout->addWidget(wordAccountBtn);

    deviceEncryptionBtn = new QPushButton("Turn Off Device Encryption", this);
    sidebarLayout->addWidget(deviceEncryptionBtn);

    hardwareTestBtn = new QPushButton("Hardware Test", this);
    sidebarLayout->addWidget(hardwareTestBtn);

    QFrame *line2 = new QFrame();
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    line2->setStyleSheet("background-color: #E0E0E0; color: #E0E0E0;");
    sidebarLayout->addWidget(line2);

    // Camera / sound / touch launchers moved into the dedicated Hardware Test tab.

    sidebarLayout->addStretch();

    // Finish the scrollable button area; the language selector stays pinned below.
    sidebarScroll->setWidget(btnHost);
    sidebarOuter->addWidget(sidebarScroll, 1);

    langCombo = new QComboBox(this);
    langCombo->addItem("English", "en");
    langCombo->addItem("Francais", "fr");
    langCombo->addItem("简体中文", "zh");
    // Reflect the auto-detected system language in the selector.
    {
        int idx = langCombo->findData(currentLang);
        if (idx >= 0) langCombo->setCurrentIndex(idx);
    }
    sidebarOuter->addWidget(langCombo);

    versionLabel = new QLabel(this);
    versionLabel->setObjectName("VersionLabel");
    versionLabel->setAlignment(Qt::AlignCenter);
    sidebarOuter->addWidget(versionLabel);

    mainLayout->addWidget(sidebar);

    // Slim always-visible handle that collapses/expands the sidebar — frees
    // ~260px for the content area, which the Hardware Test tab needs on
    // small screens.
    sidebarToggleBtn = new QToolButton(this);
    sidebarToggleBtn->setObjectName("SidebarToggle");
    sidebarToggleBtn->setArrowType(Qt::LeftArrow);
    sidebarToggleBtn->setFixedWidth(14);
    sidebarToggleBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    sidebarToggleBtn->setCursor(Qt::PointingHandCursor);
    sidebarToggleBtn->setAutoRaise(true);
    connect(sidebarToggleBtn, &QToolButton::clicked, this, [this]() {
        m_sidebarUserToggled = true;
        setSidebarCollapsed(sidebar->isVisible());
    });
    mainLayout->addWidget(sidebarToggleBtn);

    // --- Content Area ---
    QWidget *content = new QWidget(this);
    content->setObjectName("ContentArea");
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(30, 30, 30, 30);
    contentLayout->setSpacing(15);

    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 100);
    progressBar->setValue(0);
    progressBar->setFixedHeight(6);
    progressBar->setTextVisible(false);
    contentLayout->addWidget(progressBar);

    stepLabel = new QLabel("Idle", this);
    stepLabel->setAlignment(Qt::AlignRight);
    stepLabel->setStyleSheet("color: #5C7C98; font-weight: bold;");
    contentLayout->addWidget(stepLabel);

    tabs = new QTabWidget(this);
    contentLayout->addWidget(tabs, 1);

    systemInfoTab = new SystemInfoWidget(this);
    tabs->addTab(systemInfoTab, "System Summary");

    QFont monoFont("Consolas");
    monoFont.setStyleHint(QFont::Monospace);
    monoFont.setPointSize(10);

    storageTree = new QTreeWidget(this);
    storageTree->setHeaderLabels({"Property", "Value"});
    storageTree->setContextMenuPolicy(Qt::CustomContextMenu);
    storageTree->setAlternatingRowColors(false); // Disabled alternating colors
    storageTree->setIndentation(20);
    storageTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    storageTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tabs->addTab(storageTree, "Storage Details");

    motherboardTree = new QTreeWidget(this);
    motherboardTree->setHeaderLabels({"Property", "Value"});
    motherboardTree->setAlternatingRowColors(false); // Disabled alternating colors
    motherboardTree->setIndentation(20);
    motherboardTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    motherboardTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    tabs->addTab(motherboardTree, "Motherboard & BIOS");

    // Troubleshoot tab: fixes for problems that the one-click sidebar buttons
    // don't always resolve on their own. Each fix is a titled card with a short
    // explanation and an action button.
    troubleshootTab = new QWidget(this);
    auto *tsLayout = new QVBoxLayout(troubleshootTab);
    tsLayout->setContentsMargins(20, 20, 20, 20);
    tsLayout->setSpacing(8);

    fixTimeTitle = new QLabel("Time zone / clock still wrong", troubleshootTab);
    QFont tsTitleFont = fixTimeTitle->font();
    tsTitleFont.setPointSize(tsTitleFont.pointSize() + 2);
    tsTitleFont.setBold(true);
    fixTimeTitle->setFont(tsTitleFont);
    tsLayout->addWidget(fixTimeTitle);

    fixTimeDesc = new QLabel(troubleshootTab);
    fixTimeDesc->setWordWrap(true);
    fixTimeDesc->setText(
        "If \"Auto Time Zone & Sync\" didn't fix the clock, click below. This "
        "switches the Windows time server to Google (time.google.com), turns the "
        "automatic time zone back on, and forces a fresh sync.");
    tsLayout->addWidget(fixTimeDesc);

    fixTimeServerBtn = new QPushButton("Use Google Time Server && Fix Time Zone", troubleshootTab);
    fixTimeServerBtn->setMinimumHeight(36);
    tsLayout->addWidget(fixTimeServerBtn, 0, Qt::AlignLeft);

    tsLayout->addSpacing(16);

    hpHotkeyTitle = new QLabel("HP Hotkey", troubleshootTab);
    hpHotkeyTitle->setFont(tsTitleFont);
    tsLayout->addWidget(hpHotkeyTitle);

    hpHotkeyDesc = new QLabel(troubleshootTab);
    hpHotkeyDesc->setWordWrap(true);
    tsLayout->addWidget(hpHotkeyDesc);

    hpHotkeyBtn = new QPushButton("HP Hotkey", troubleshootTab);
    hpHotkeyBtn->setMinimumHeight(36);
    tsLayout->addWidget(hpHotkeyBtn, 0, Qt::AlignLeft);

    tsLayout->addSpacing(16);

    lenovoVantageTitle = new QLabel("Lenovo Commercial Vantage", troubleshootTab);
    lenovoVantageTitle->setFont(tsTitleFont);
    tsLayout->addWidget(lenovoVantageTitle);

    lenovoVantageDesc = new QLabel(troubleshootTab);
    lenovoVantageDesc->setWordWrap(true);
    tsLayout->addWidget(lenovoVantageDesc);

    lenovoVantageBtn = new QPushButton("Lenovo Commercial Vantage", troubleshootTab);
    lenovoVantageBtn->setMinimumHeight(36);
    tsLayout->addWidget(lenovoVantageBtn, 0, Qt::AlignLeft);

    tsLayout->addStretch(1);
    tabs->addTab(troubleshootTab, "Troubleshoot");

    // Hardware tester opens in a separate window from the sidebar.
    hardwareTestDialog = new QDialog(this);
    hardwareTestDialog->setWindowIcon(windowIcon());
    hardwareTestDialog->setModal(false);
    hardwareTestDialog->resize(1100, 750);
    hardwareTestDialog->setMinimumSize(850, 600);
    auto *hardwareTestLayout = new QVBoxLayout(hardwareTestDialog);
    hardwareTestLayout->setContentsMargins(0, 0, 0, 0);
    hardwareTestTab = new HardwareTesterWidget(
        [this](const QString &k) { return getTranslation(k); }, hardwareTestDialog);
    hardwareTestLayout->addWidget(hardwareTestTab);

    QHBoxLayout *logLayout = new QHBoxLayout();
    logLayout->addStretch();

    copyToDesktopBtn = new QPushButton("Copy to Desktop", this);
    copyToDesktopBtn->setObjectName("LogButton"); // Reuse style
    logLayout->addWidget(copyToDesktopBtn);

    deleteDistBtn = new QPushButton("Delete App", this);
    deleteDistBtn->setObjectName("LogButton"); // Reuse style
    deleteDistBtn->setStyleSheet("background-color: #C0392B; color: white;"); // Red for delete
    logLayout->addWidget(deleteDistBtn);

    // --- Visibility Logic for Desktop Buttons ---
    QString appDir = QDir::cleanPath(HardwareModel::getBaseDir());
    QString desktopPath = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));

    // Check if app is on desktop (directly or in a subfolder)
    bool isOnDesktop = false;
    if (appDir.compare(desktopPath, Qt::CaseInsensitive) == 0) {
        isOnDesktop = true;
    } else {
        QDir dir(appDir);
        if (dir.cdUp()) {
            QString parentDir = QDir::cleanPath(dir.absolutePath());
            if (parentDir.compare(desktopPath, Qt::CaseInsensitive) == 0) {
                isOnDesktop = true;
            }
        }
    }

    if (isOnDesktop) {
        copyToDesktopBtn->setVisible(false);
        deleteDistBtn->setVisible(true);
    } else {
        copyToDesktopBtn->setVisible(true);
        deleteDistBtn->setVisible(false);
    }
    // ---------------------------------------------

    logToggleBtn = new QPushButton("Show Log", this);
    logToggleBtn->setFixedWidth(120);
    logToggleBtn->setObjectName("LogButton");
    logToggleBtn->setVisible(false);
    logLayout->addWidget(logToggleBtn);
    contentLayout->addLayout(logLayout);

    logOutput = new QPlainTextEdit(this);
    logOutput->setReadOnly(true);
    logOutput->setMaximumHeight(100);
    logOutput->setFont(monoFont);
    logOutput->setVisible(false);
    contentLayout->addWidget(logOutput);

    mainLayout->addWidget(content, 1);
    setCentralWidget(central);

    statusBar = new QStatusBar(this);
    setStatusBar(statusBar);
}

// Shows/hides the sidebar and flips the arrow + tooltip of the handle button.
void MainWindow::setSidebarCollapsed(bool collapsed) {
    sidebar->setVisible(!collapsed);
    sidebarToggleBtn->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
    sidebarToggleBtn->setToolTip(
        getTranslation(collapsed ? "sidebar_expand" : "sidebar_collapse"));
}

// Connects every button/combo/tree signal to its handler slot.
void MainWindow::connectSignals() {
    connect(refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshClicked);
    connect(officeBtn, &QPushButton::clicked, this, &MainWindow::onInstallSoftwaresClicked);
    connect(openUpdatesBtn, &QPushButton::clicked, this, &MainWindow::onUpdatesClicked);
    connect(syncTimeBtn, &QPushButton::clicked, this, &MainWindow::onSyncTimeClicked);
    connect(fixTimeServerBtn, &QPushButton::clicked, this, &MainWindow::onFixTimeServerClicked);
    connect(hpHotkeyBtn, &QPushButton::clicked, this, &MainWindow::onHpHotkeyFixClicked);
    connect(lenovoVantageBtn, &QPushButton::clicked, this, &MainWindow::onLenovoVantageClicked);
    connect(officeFolderBtn, &QPushButton::clicked, this, &MainWindow::onCreateOfficeFolderClicked);
    connect(deviceEncryptionBtn, &QPushButton::clicked, this, &MainWindow::onDeviceEncryptionClicked);
    connect(hardwareTestBtn, &QPushButton::clicked, this, [this]() {
        hardwareTestDialog->show();
        hardwareTestDialog->raise();
        hardwareTestDialog->activateWindow();
    });
    connect(wordAccountBtn, &QPushButton::clicked, this, &MainWindow::onWordAccountClicked);
    connect(langCombo, &QComboBox::currentTextChanged, [this](const QString &text){
        onLanguageChanged(langCombo->currentData().toString());
    });
    connect(logToggleBtn, &QPushButton::clicked, this, &MainWindow::onToggleLog);
    connect(copyToDesktopBtn, &QPushButton::clicked, this, &MainWindow::onCopyToDesktopClicked);
    connect(deleteDistBtn, &QPushButton::clicked, this, &MainWindow::onDeleteDistClicked);
    connect(storageTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onStorageContextMenu);
}

// Copies the portable package (launcher exe + QuickReportsDist folder) to the
// user's Desktop. The launcher is copied directly; the large dist folder is
// mirrored with robocopy in a child process so the UI stays responsive.
void MainWindow::onCopyToDesktopClicked() {
    QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QString appDir = HardwareModel::getBaseDir(); // This is QuickReportsDist if running from there

    // If running from launcher, we want to copy the launcher and the dist folder
    // Assuming structure: Root/QuickReports.exe and Root/QuickReportsDist/

    QDir dir(appDir);
    QString rootDir;
    QString distFolderName;

    if (dir.dirName() == "QuickReportsDist") {
        distFolderName = "QuickReportsDist";
        dir.cdUp();
        rootDir = dir.absolutePath();
    } else {
        // Running from IDE or weird structure
        rootDir = appDir;
        distFolderName = ""; // Just copy current dir?
    }

    QString launcherPath = rootDir + "/QuickReports.exe";
    QString distPath = rootDir + "/QuickReportsDist";

    if (!QFile::exists(launcherPath) || !QDir(distPath).exists()) {
        QMessageBox::warning(this, "Copy Failed", "Could not find QuickReports.exe or QuickReportsDist folder in: " + rootDir);
        return;
    }

    // Copy Launcher (a single small exe — a plain copy is instant).
    QString destLauncher = desktopPath + "/QuickReports.exe";
    if (QFile::exists(destLauncher)) QFile::remove(destLauncher);
    if (!QFile::copy(launcherPath, destLauncher)) {
        QMessageBox::warning(this, "Copy Failed", "Failed to copy launcher to Desktop.");
        return;
    }

    // Copy the dist folder with robocopy instead of a file-by-file QFile::copy
    // loop. The dist folder is hundreds of small Qt runtime files, where the
    // per-file overhead dominates; robocopy's multithreaded copy (/MT) is far
    // faster, and running it as a separate process keeps the UI responsive.
    // /MIR mirrors the source, so there is no need to delete the old copy first.
    QString destDist = desktopPath + "/QuickReportsDist";

    auto *dlg = new QProgressDialog("Copying to Desktop…", QString(), 0, 0, this);
    dlg->setWindowTitle("Copy to Desktop");
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setMinimumDuration(0);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setCancelButton(nullptr);
    dlg->setValue(0);

    auto *proc = new QProcess(this);
    // Both finished() and errorOccurred() can fire (e.g. a crash emits both);
    // this flag makes sure the result is reported and the objects freed once.
    auto handled = std::make_shared<bool>(false);
    auto finish = [this, proc, dlg, handled](bool ok) {
        if (*handled) return;
        *handled = true;
        dlg->close();
        dlg->deleteLater();
        proc->deleteLater();
        if (ok)
            QMessageBox::information(this, "Success", "Application copied to Desktop successfully.");
        else
            QMessageBox::warning(this, "Copy Failed", "Failed to copy distribution folder.");
    };
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [finish](int exitCode, QProcess::ExitStatus status) {
        // robocopy uses exit codes as bit flags: 0–7 mean success (files copied,
        // extras purged, etc.); 8 and above indicate at least one real failure.
        finish(status == QProcess::NormalExit && exitCode < 8);
    });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError) {
        finish(false);
    });

    dlg->show();
    proc->start("robocopy", QStringList()
        << QDir::toNativeSeparators(distPath)
        << QDir::toNativeSeparators(destDist)
        << "/MIR" << "/MT:16" << "/NFL" << "/NDL" << "/NJH" << "/NJS" << "/NP"
        << "/R:1" << "/W:1");
}

// Uninstalls the portable copy: after confirmation writes a temporary batch file
// that waits for this process to exit and then deletes the launcher and dist
// folder (a running exe cannot delete itself), launches it detached and quits.
void MainWindow::onDeleteDistClicked() {
    if (QMessageBox::question(this, "Confirm Delete", "Are you sure you want to delete this application and all its files?\nThe application will close immediately.", QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString appDir = HardwareModel::getBaseDir();
    QDir dir(appDir);
    QString rootDir;

    if (dir.dirName() == "QuickReportsDist") {
        dir.cdUp();
        rootDir = dir.absolutePath();
    } else {
        rootDir = appDir;
    }

    QString launcherPath = QDir::toNativeSeparators(rootDir + "/QuickReports.exe");
    QString distPath = QDir::toNativeSeparators(rootDir + "/QuickReportsDist");

    dismountOfficeImage();

    // Create a self-deleting batch script
    QString scriptPath = QDir::temp().filePath("self_delete.bat");
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&script);
        out << "@echo off\n";
        out << "timeout /t 2 /nobreak > NUL\n"; // Wait for app to close
        out << "del \"" << launcherPath << "\"\n";
        out << "rmdir /s /q \"" << distPath << "\"\n";
        out << "del \"%~f0\"\n"; // Delete script itself
        script.close();

        QProcess::startDetached("cmd.exe", QStringList() << "/c" << scriptPath);
        QApplication::quit();
    } else {
        QMessageBox::critical(this, "Error", "Failed to create deletion script.");
    }
}


// Starts a full hardware scan: resets progress/UI, tears down the previous worker
// thread + model, creates a fresh HardwareModel on a new QThread, connects its
// progressive signals and starts it. Also schedules the Windows Update check.
void MainWindow::onRefreshClicked() {
    refreshBtn->setEnabled(false);
    progressBar->setValue(0);
    stepLabel->setText("Starting hardware scan...");

    // Stream cards into the summary tab as each piece of data arrives instead of
    // holding a placeholder until the whole scan finishes.
    const QString scanningMsg = getTranslation("scanning_placeholder");
    systemInfoTab->beginProgressive(currentLang);
    storageTree->clear();
    new QTreeWidgetItem(storageTree, {scanningMsg, ""});
    motherboardTree->clear();
    new QTreeWidgetItem(motherboardTree, {scanningMsg, ""});

    // Clean up previous thread/model if they exist
    if (workerThread) {
        workerThread->quit();
        workerThread->wait();
        delete workerThread;
        workerThread = nullptr;
    }
    if (hardwareModel) {
        delete hardwareModel;
        hardwareModel = nullptr;
    }

    workerThread = new QThread;
    hardwareModel = new HardwareModel;
    hardwareModel->moveToThread(workerThread);

    connect(workerThread, &QThread::started, hardwareModel, &HardwareModel::runScan);
    connect(hardwareModel, &HardwareModel::progress, this, &MainWindow::updateProgress);
    connect(hardwareModel, &HardwareModel::cardReady, this, &MainWindow::onCardReady);
    connect(hardwareModel, &HardwareModel::storageReady, this, &MainWindow::onStorageReady);
    connect(hardwareModel, &HardwareModel::motherboardReady, this, &MainWindow::onMotherboardReady);
    connect(hardwareModel, &HardwareModel::batteryReady, this, &MainWindow::onBatteryReady);
    connect(hardwareModel, &HardwareModel::finished, this, &MainWindow::onScanFinished);
    connect(hardwareModel, &HardwareModel::error, this, &MainWindow::onScanError);

    // Driver check finishes on its own, after everything else is displayed.
    connect(hardwareModel, &HardwareModel::driverResult, this, [this](const QString &text) {
        appendLog(text);
    });


    // Only quit the thread when finished, do NOT deleteLater the objects automatically
    // We manage their lifecycle in onRefreshClicked and ~MainWindow
    connect(hardwareModel, &HardwareModel::finished, workerThread, &QThread::quit);

    workerThread->start();

    // Defer the Windows Update check so its PowerShell process doesn't compete
    // with the hardware scan for CPU/IO on slow machines.
    QTimer::singleShot(2500, this, [this]() { checkForUpdates(); });
}

// Slot for HardwareModel::progress - updates the bar, the translated step label,
// the log panel and the splash message.
void MainWindow::updateProgress(int current, int total, const QString &label) {
    int value = (total > 0) ? (current * 100 / total) : 0;
    progressBar->setValue(value);

    QString trLabel = getTranslation(label);
    if (trLabel.isEmpty()) trLabel = label;

    QString stepText = getTranslation("step_label");
    stepText.replace("{current}", QString::number(current));
    stepText.replace("{total}", QString::number(total));
    stepText.replace("{label}", trLabel);

    stepLabel->setText(stepText);
    appendLog(stepText);

    // Update splash screen message if visible
    if (splashScreen && splashScreen->isVisible()) {
        splashScreen->showMessage(trLabel + "...", Qt::AlignBottom | Qt::AlignRight, Qt::black);
    }
}

// Slot for the final scan result: caches it (for re-rendering on language
// change), renders all tabs, then schedules the display-info step.
void MainWindow::onScanFinished(const QString &title, const QString &summary, const QList<StorageDisk> &storage, const QMap<QString, QString> &motherboard, const QList<BatteryInfo> &battery, const SystemSummaryData &overview) {
    // Cache results so we can re-render in another language without re-scanning.
    // (Safe even when re-invoked with the members themselves — QList/QString
    // self-assignment is a no-op.)
    m_scanTitle = title;
    m_scanSummary = summary;
    m_scanStorage = storage;
    m_scanMotherboard = motherboard;
    m_scanBattery = battery;
    m_scanOverview = overview;
    m_hasScan = true;

    setWindowTitle(title);

    // Render all the other info at once now. The display step runs last and is
    // shown as its own stage below (completed in onDisplayReady).
    systemInfoTab->updateData(overview, storage, battery, motherboard, currentLang);
    populateStorageTree(storage);
    populateMotherboardTree(motherboard);

    progressBar->setValue(92);
    stepLabel->setText(getTranslation("reading_display"));
    appendLog(getTranslation("reading_display"));

    refreshBtn->setEnabled(true);

    // Now that the other info is on screen, read the display info (short delay so
    // the rendering above is actually painted first), then fill in the card.
    QTimer::singleShot(300, this, &MainWindow::detectDisplayInfo);
    statusBar->showMessage(getTranslation("status_updated"), 3000);

    // Close splash screen if active
    if (splashScreen) {
        this->show(); // Ensure main window is visible
        splashScreen->finish(this);
        splashScreen = nullptr; // Don't delete here, main.cpp owns it or it deletes on close
    }
}

// Rebuilds the Storage tab tree (disks -> properties, SMART health, partitions).
// The previous expansion state, selection and scroll position are captured
// before and restored after, so automatic refreshes do not make the view jump.
// Item user-data roles carry disk number / flags for the context menu
// (UserRole: disk number, +1 file system, +2 isEmpty, +3 unallocated bytes,
// +4 offline, +5 WinRE status).
void MainWindow::populateStorageTree(const QList<StorageDisk> &storage) {
    // Smooth refresh: an automatic re-populate (after a clone, or when a disk is
    // plugged/unplugged) must not collapse and jump the whole tree. Remember
    // which nodes are expanded, what is selected and the scroll position, then
    // restore them after the rebuild — all within this one synchronous call, so
    // no intermediate empty state is ever painted and the view feels untouched.
    auto itemPath = [](QTreeWidgetItem *it) -> QString {
        QStringList parts;
        for (QTreeWidgetItem *n = it; n; n = n->parent())
            parts.prepend(n->text(0));
        return parts.join(QChar(0x1f));
    };
    QSet<QString> expandedPaths;
    QSet<QString> knownDisks;
    QString selectedPath;
    // Only preserve when the tree already holds real disks (not the first scan's
    // "scanning…" placeholder, whose rows carry no disk number).
    const bool hadPriorState = storageTree->topLevelItemCount() > 0 &&
        !storageTree->topLevelItem(0)->data(0, Qt::UserRole).toString().isEmpty();
    if (hadPriorState) {
        std::function<void(QTreeWidgetItem*)> capture = [&](QTreeWidgetItem *it) {
            if (it->isExpanded())
                expandedPaths.insert(itemPath(it));
            for (int c = 0; c < it->childCount(); ++c)
                capture(it->child(c));
        };
        for (int i = 0; i < storageTree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *top = storageTree->topLevelItem(i);
            capture(top);
            const QString dn = top->data(0, Qt::UserRole).toString();
            if (!dn.isEmpty())
                knownDisks.insert(dn);
        }
        if (QTreeWidgetItem *sel = storageTree->currentItem())
            selectedPath = itemPath(sel);
    }
    const int scrollValue = storageTree->verticalScrollBar()->value();

    storageTree->setUpdatesEnabled(false);
    storageTree->clear();
    systemDiskNum.clear();

    // Fallback system drive detection for clone source selection.
    QString systemDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
    if (systemDrive.size() >= 2 && systemDrive[1] == ':')
        systemDrive = systemDrive.left(2).toUpper();

    QString inferredSystemDiskNum;
    if (!systemDrive.isEmpty()) {
        for (const auto &disk : storage) {
            if (disk.diskNumber.isEmpty()) continue;
            bool hostsSystemDrive = false;
            for (const auto &part : disk.partitions) {
                if (part.deviceID.startsWith(systemDrive, Qt::CaseInsensitive)) {
                    hostsSystemDrive = true;
                    break;
                }
            }
            if (hostsSystemDrive) { inferredSystemDiskNum = disk.diskNumber; break; }
        }
    }

    for (const auto &disk : storage) {
        QTreeWidgetItem *diskItem = new QTreeWidgetItem(storageTree);
        diskItem->setText(0, "Disk " + disk.deviceID);
        diskItem->setText(1, disk.model);
        diskItem->setExpanded(true);

        diskItem->setData(0, Qt::UserRole, disk.diskNumber);
        diskItem->setData(0, Qt::UserRole + 2, disk.isEmpty);
        diskItem->setData(0, Qt::UserRole + 3, static_cast<qulonglong>(disk.unallocatedSpace));
        diskItem->setData(0, Qt::UserRole + 4, disk.isOffline);
        diskItem->setData(0, Qt::UserRole + 5, disk.winReStatus);

        const bool isSystemDisk = disk.isSystem || (!inferredSystemDiskNum.isEmpty() && disk.diskNumber == inferredSystemDiskNum);
        if (isSystemDisk) {
            systemDiskNum = disk.diskNumber;
            diskItem->setText(0, diskItem->text(0) + " (" + getTranslation("disk_system") + ")");
            QFont f = diskItem->font(0);
            f.setBold(true);
            diskItem->setFont(0, f);
        }

        // Model is skipped here — the disk's top-level row already shows it.
        {
            QStringList ifParts;
            if (!disk.interfaceType.trimmed().isEmpty()) ifParts << disk.interfaceType.trimmed();
            if (!disk.mediaType.trimmed().isEmpty())     ifParts << disk.mediaType.trimmed();
            new QTreeWidgetItem(diskItem, {getTranslation("disk_interface"), ifParts.join(" · ")});
        }
        new QTreeWidgetItem(diskItem, {getTranslation("disk_size"), QString::number(disk.size / (1024.0*1024*1024), 'f', 2) + " GB"});

        if (disk.isOffline) {
            QTreeWidgetItem *statusItem = new QTreeWidgetItem(diskItem, {getTranslation("disk_status"), getTranslation("disk_offline")});
            statusItem->setForeground(1, QBrush(QColor("#cc7a00")));
            QFont f = statusItem->font(1);
            f.setBold(true);
            statusItem->setFont(1, f);
        } else if (disk.isEmpty) {
            QTreeWidgetItem *statusItem = new QTreeWidgetItem(diskItem, {getTranslation("disk_status"), getTranslation("disk_empty")});
            statusItem->setForeground(1, QBrush(Qt::red));
        } else {
            new QTreeWidgetItem(diskItem, {getTranslation("disk_status"), getTranslation("disk_active")});
        }

        if (disk.unallocatedSpace > 0) {
             new QTreeWidgetItem(diskItem, {getTranslation("disk_unallocated"), QString::number(disk.unallocatedSpace / (1024.0*1024*1024), 'f', 2) + " GB"});
        }

        // Windows Recovery (WinRE) status on the system disk (and wherever WinRE
        // physically lives). "Disabled" is the cue for the Restore WinRE action.
        if (!disk.winReStatus.isEmpty()) {
            const bool enabled = disk.winReStatus == "Enabled";
            QTreeWidgetItem *wr = new QTreeWidgetItem(diskItem, {getTranslation("disk_winre"),
                getTranslation(enabled ? "winre_enabled" : "winre_disabled")});
            wr->setForeground(1, QBrush(enabled ? QColor("#1a7a1a") : QColor("#cc7a00")));
        }

        // SMART health section
        {
            const SmartHealthInfo &sm = disk.smart;
            QTreeWidgetItem *smartItem = new QTreeWidgetItem(diskItem);
            smartItem->setText(0, getTranslation("smart_health"));

            if (!sm.valid) {
                smartItem->setText(1, getTranslation(
                    sm.usbBus ? "smart_unavailable_usb" : "smart_unavailable"));
                smartItem->setForeground(1, QBrush(QColor("#6a7a8a")));
            } else {
                QString verdictKey = sm.verdict == "Good" ? "smart_good"
                                   : sm.verdict == "Warning" ? "smart_warning" : "smart_critical";
                QColor verdictColor = sm.verdict == "Good" ? QColor("#1a7a1a")
                                    : sm.verdict == "Warning" ? QColor("#cc7a00") : QColor("#cc2222");
                QString verdictLine = getTranslation(verdictKey);
                if (sm.healthPercent >= 0)
                    verdictLine += QString(" %1%").arg(sm.healthPercent);
                verdictLine += " (" + sm.protocol + ")";
                smartItem->setText(1, verdictLine);
                smartItem->setForeground(1, QBrush(verdictColor));
                QFont f = smartItem->font(1);
                f.setBold(true);
                smartItem->setFont(1, f);

                if (sm.temperatureC >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_temperature"), QString::number(sm.temperatureC) + " °C"});
                if (sm.powerOnHours >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_power_on_hours"), QString::number(sm.powerOnHours) + " h"});
                if (sm.powerCycles >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_power_cycles"), QString::number(sm.powerCycles)});
                if (sm.percentageUsed >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_wear"), QString::number(sm.percentageUsed) + " %"});
                if (sm.availableSpare >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_spare"), QString::number(sm.availableSpare) + " %"});
                if (sm.hostWrittenTb >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_written"), QString::number(sm.hostWrittenTb, 'f', 1) + " TB"});
                if (sm.reallocatedSectors >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_realloc"), QString::number(sm.reallocatedSectors)});
                if (sm.pendingSectors >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_pending"), QString::number(sm.pendingSectors)});
                if (sm.uncorrectableSectors >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_uncorrectable"), QString::number(sm.uncorrectableSectors)});
                if (sm.mediaErrors >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_media_errors"), QString::number(sm.mediaErrors)});
                if (sm.unsafeShutdowns >= 0)
                    new QTreeWidgetItem(smartItem, {getTranslation("smart_unsafe_shutdowns"), QString::number(sm.unsafeShutdowns)});
                if (!sm.issues.isEmpty()) {
                    QTreeWidgetItem *issueItem = new QTreeWidgetItem(smartItem, {getTranslation("smart_issues"), sm.issues.join("; ")});
                    issueItem->setForeground(1, QBrush(verdictColor));
                }
            }
        }

        if (!disk.partitions.isEmpty()) {
            QTreeWidgetItem *partsItem = new QTreeWidgetItem(diskItem, {getTranslation("disk_partitions"), ""});
            for (const auto &part : disk.partitions) {
                QString partText = part.deviceID;
                if (!part.volumeName.isEmpty()) partText += " (" + part.volumeName + ")";
                if (part.fileSystem != "Unknown") partText += " [" + part.fileSystem + "]";

                QString details;
                if (part.isLogical && part.size > 0) {
                    const double totalGb = part.size / (1024.0 * 1024 * 1024);
                    details = QString("%1 GB").arg(totalGb, 0, 'f', 2);
                } else {
                    const qint64 raw = (part.physicalSize > 0) ? part.physicalSize : part.size;
                    const double physGb = raw / (1024.0 * 1024 * 1024);
                    details = QString("%1 GB (%2)").arg(physGb, 0, 'f', 2).arg(getTranslation("disk_partition_suffix"));
                }

                QTreeWidgetItem *pItem = new QTreeWidgetItem(partsItem, {partText, details});
                pItem->setData(0, Qt::UserRole, disk.diskNumber);
                pItem->setData(1, Qt::UserRole, part.partitionNum);
                pItem->setData(0, Qt::UserRole + 1, part.fileSystem);
            }
        }
    }
    storageTree->resizeColumnToContents(0);

    // Restore the pre-refresh view. Disks that existed before keep the viewer's
    // exact expand/collapse choices; a newly attached disk keeps its default
    // (expanded) so it draws the eye. Selection and scroll position return too.
    if (hadPriorState) {
        std::function<void(QTreeWidgetItem*)> restore = [&](QTreeWidgetItem *it) {
            const QString path = itemPath(it);
            QTreeWidgetItem *top = it;
            while (top->parent())
                top = top->parent();
            const bool wasKnown = knownDisks.contains(top->data(0, Qt::UserRole).toString());
            // Only override the freshly-built defaults for disks we saw before;
            // leave new disks at their default state.
            if (wasKnown && (it->parent() == nullptr || it->childCount() > 0))
                it->setExpanded(expandedPaths.contains(path));
            if (!selectedPath.isEmpty() && path == selectedPath) {
                it->setSelected(true);
                storageTree->setCurrentItem(it);
            }
            for (int c = 0; c < it->childCount(); ++c)
                restore(it->child(c));
        };
        for (int i = 0; i < storageTree->topLevelItemCount(); ++i)
            restore(storageTree->topLevelItem(i));
    }
    storageTree->setUpdatesEnabled(true);
    storageTree->verticalScrollBar()->setValue(scrollValue);
}

// Fills the Motherboard & BIOS tab from a key/value map.
void MainWindow::populateMotherboardTree(const QMap<QString, QString> &motherboard) {
    motherboardTree->clear();
    QMapIterator<QString, QString> i(motherboard);
    while (i.hasNext()) {
        i.next();
        new QTreeWidgetItem(motherboardTree, {i.key(), i.value()});
    }
    motherboardTree->resizeColumnToContents(0);
}

// Progressive scan slots: each stores the latest snapshot (for language
// re-render) and adds the matching card/tree content immediately.
void MainWindow::onCardReady(const QString &stage, const SystemSummaryData &overview) {
    systemInfoTab->addCard(stage, overview);
    m_scanOverview = overview; // keep latest snapshot for language re-render
    m_hasScan = true;
}

void MainWindow::onStorageReady(const QList<StorageDisk> &storage) {
    m_scanStorage = storage;
    m_hasScan = true;
    systemInfoTab->addStorageCard(storage);
    populateStorageTree(storage);
}

void MainWindow::onMotherboardReady(const QMap<QString, QString> &motherboard) {
    m_scanMotherboard = motherboard;
    populateMotherboardTree(motherboard);
}

void MainWindow::onBatteryReady(const QList<BatteryInfo> &battery) {
    m_scanBattery = battery;
    systemInfoTab->addBatteryCards(battery);
}

// Reads resolution/refresh rate of every attached monitor and touch capability.
// Done on the GUI thread after the rest has rendered (display APIs prefer it).
void MainWindow::detectDisplayInfo() {
    if (!m_hasScan) return;

    QElapsedTimer stepTimer;
    stepTimer.start();

    #ifdef Q_OS_WIN
    QList<DisplayEntry> displays;
    DISPLAY_DEVICEW dd;
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
            continue;
        DEVMODEW dm;
        dm.dmSize = sizeof(dm);
        dm.dmDriverExtra = 0;
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            DisplayEntry de;
            de.width       = static_cast<int>(dm.dmPelsWidth);
            de.height      = static_cast<int>(dm.dmPelsHeight);
            de.refreshRate = static_cast<int>(dm.dmDisplayFrequency);
            displays.append(de);
        }
    }
    int digitizer = GetSystemMetrics(SM_DIGITIZER);
    bool ready = (digitizer & NID_READY) != 0;
    bool touch = (digitizer & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH)) != 0;
    m_scanOverview.hasTouch = (ready && touch) || GetSystemMetrics(SM_MAXIMUMTOUCHES) > 0;
    m_scanOverview.displays = displays;
    #endif

    m_scanOverview.displayReady = true;
    CrashHandler::log(QString("Read display information in %1 ms").arg(stepTimer.elapsed()));

    // Re-render so the Display card fills in.
    systemInfoTab->updateData(m_scanOverview, m_scanStorage, m_scanBattery,
                              m_scanMotherboard, currentLang);

    // Scan is now fully complete.
    progressBar->setValue(100);
    stepLabel->setText(getTranslation("scan_finished"));
    statusBar->showMessage(getTranslation("status_updated"), 3000);
}


// Reports a failed scan: resets progress, logs, re-enables Refresh, shows a dialog.
void MainWindow::onScanError(const QString &msg) {
    progressBar->setValue(0);
    stepLabel->setText("Error");
    appendLog("Error: " + msg);
    refreshBtn->setEnabled(true);
    QMessageBox::critical(this, getTranslation("error_title"), getTranslation("operation_failed").replace("{msg}", msg));

    // Close splash on error too
    if (splashScreen) {
        this->show();
        splashScreen->finish(this);
        splashScreen = nullptr;
    }
}



// User picked another language: retranslate widgets and re-render cached data.
void MainWindow::onLanguageChanged(const QString &langCode) {
    currentLang = langCode;
    retranslateUi();

    // Re-render the cached hardware info in the newly selected language in one shot
    // (all data is already available, so no need to stream).
    if (m_hasScan) {
        systemInfoTab->updateData(m_scanOverview, m_scanStorage, m_scanBattery,
                                  m_scanMotherboard, currentLang);
        populateStorageTree(m_scanStorage);
        populateMotherboardTree(m_scanMotherboard);
    }
}

// Shows/hides the log panel and updates the toggle button caption.
void MainWindow::onToggleLog() {
    bool visible = logOutput->isVisible();
    logOutput->setVisible(!visible);
    logToggleBtn->setText(getTranslation(visible ? "show_log" : "hide_log"));
}

// Appends one line to the on-screen log panel.
void MainWindow::appendLog(const QString &msg) {
    logOutput->appendPlainText(msg);
}


// Loads all translation tables (main UI + hardware tester).
void MainWindow::loadTranslations() {
    // UI strings live in a single key -> {en, fr, zh} table in Translations.cpp,
    // so a new string is added in one place instead of three per-language maps.
    loadQuickReportsTranslations(translations);
    // Hardware-tester module strings are maintained separately.
    addTesterTranslations(translations);
}

// Translation lookup: current language -> English -> the raw key.
QString MainWindow::getTranslation(const QString &key) {
    if (translations.contains(currentLang) && translations[currentLang].contains(key)) {
        return translations[currentLang][key];
    }
    if (translations["en"].contains(key)) {
        return translations["en"][key];
    }
    return key;
}

// Applies getTranslation() to every static widget text, tab title and header.
void MainWindow::retranslateUi() {
    const QString appVersion = QCoreApplication::applicationVersion();
    setWindowTitle(QString("%1 — v%2").arg(getTranslation("window_title"), appVersion));
    versionLabel->setText(getTranslation("version_label").replace("{version}", appVersion));
    refreshBtn->setText(getTranslation("refresh_hw_info"));
    officeBtn->setText(getTranslation("install_softwares"));
    openUpdatesBtn->setText(getTranslation("open_win_update"));
    syncTimeBtn->setText(getTranslation("sync_time"));
    officeFolderBtn->setText(getTranslation("office_folder"));
    deviceEncryptionBtn->setText(getTranslation("device_encryption"));
    hardwareTestBtn->setText(getTranslation("tab_hardware_test"));
    hardwareTestDialog->setWindowTitle(getTranslation("tab_hardware_test"));
    wordAccountBtn->setText(getTranslation("word_account"));
    if (sidebarToggleBtn)
        sidebarToggleBtn->setToolTip(getTranslation(
            sidebar->isVisible() ? "sidebar_collapse" : "sidebar_expand"));
    copyToDesktopBtn->setText(getTranslation("copy_to_desktop"));
    deleteDistBtn->setText(getTranslation("delete_app"));

    tabs->setTabText(0, getTranslation("tab_summary"));
    tabs->setTabText(1, getTranslation("tab_storage"));
    tabs->setTabText(2, getTranslation("tab_motherboard"));
    tabs->setTabText(3, getTranslation("tab_troubleshoot"));
    fixTimeTitle->setText(getTranslation("fix_time_title"));
    fixTimeDesc->setText(getTranslation("fix_time_desc"));
    fixTimeServerBtn->setText(getTranslation("fix_time_btn"));
    hpHotkeyTitle->setText(getTranslation("hp_hotkey_title"));
    hpHotkeyDesc->setText(getTranslation("hp_hotkey_desc"));
    hpHotkeyBtn->setText(getTranslation("hp_hotkey_btn"));
    lenovoVantageTitle->setText(getTranslation("lenovo_vantage_title"));
    lenovoVantageDesc->setText(getTranslation("lenovo_vantage_desc"));
    lenovoVantageBtn->setText(getTranslation("lenovo_vantage_btn"));
    storageTree->setHeaderLabels({getTranslation("col_property"), getTranslation("col_value")});
    motherboardTree->setHeaderLabels({getTranslation("col_property"), getTranslation("col_value")});
    if (hardwareTestTab)
        hardwareTestTab->retranslate();

    osLabel->setText("Detected OS: " + HardwareModel::detectOs());

    bool logVisible = logOutput->isVisible();
    logToggleBtn->setText(getTranslation(logVisible ? "hide_log" : "show_log"));
}

// Application-wide Qt style sheet (QSS): background image, sidebar, buttons,
// tabs, trees, scroll bars, combo boxes and the hardware tester's nav/cards.
// Colours: accent #5C7C98, borders #D6DEE6.
void MainWindow::applyStylesheet() {
    // Use the Qt Resource System path for the background image
    QString bgPath = ":/background_light.png";
    QString bgStyle = QString("border-image: url(%1) 0 0 0 0 stretch stretch;").arg(bgPath);

    QString qss = QString(R"(
        * { font-family: 'Segoe UI', sans-serif; }
        QMainWindow {
            %1
        }
        QWidget#CentralWidget {
            background: transparent;
        }
        QWidget#Sidebar {
            background-color: rgba(255, 255, 255, 0.94);
            border-right: 1px solid #D6DEE6;
            border-top-right-radius: 12px;
            border-bottom-right-radius: 12px;
        }
        QWidget#ContentArea {
            background: transparent;
        }
        QToolButton#SidebarToggle {
            background-color: rgba(255, 255, 255, 0.55);
            border: none;
            border-right: 1px solid #D6DEE6;
            color: #5C7C98;
        }
        QToolButton#SidebarToggle:hover {
            background-color: rgba(255, 255, 255, 0.92);
        }
        QPushButton {
            background-color: #5C7C98;
            color: white;
            border: none;
            padding: 8px 14px;
            border-radius: 8px;
            font-weight: 600;
            font-size: 12px;
        }
        QPushButton:hover {
            background-color: #4A6A85;
        }
        QPushButton:pressed {
            background-color: #38556E;
        }
        QPushButton:disabled {
            background-color: #B9C4CE;
            color: #EAEFF3;
        }
        QWidget#HardwareTester QPushButton {
            color: #000000;
        }
        QPushButton#LogButton {
            background-color: #88B2CC;
            color: white;
        }
        QPushButton#LogButton:hover {
            background-color: #769EBD;
        }
        QTabWidget::pane {
            border: 1px solid #D6DEE6;
            border-radius: 10px;
            background-color: rgba(255, 255, 255, 0.96);
            top: -1px;
        }
        QTabBar::tab {
            background: #E8ECEF;
            color: #5A6673;
            padding: 9px 22px;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
            border: 1px solid #D6DEE6;
            border-bottom: none;
            margin-right: 3px;
        }
        QTabBar::tab:selected {
            background: #5C7C98;
            color: white;
            font-weight: 600;
        }
        QTabBar::tab:hover:!selected {
            background-color: #DBE2E8;
        }
        QPlainTextEdit, QTreeWidget {
            background-color: rgba(255, 255, 255, 0.94);
            border: 1px solid #D6DEE6;
            border-radius: 8px;
            color: #2a3a4a;
            font-size: 13px;
            selection-background-color: #5C7C98;
            selection-color: white;
            outline: none;
        }
        QPlainTextEdit {
            font-family: 'Consolas', monospace;
        }
        QTreeWidget::item {
            padding: 5px 3px;
            border-bottom: 1px solid #EEF2F5;
        }
        QTreeWidget::item:hover {
            background-color: rgba(92, 124, 152, 0.10);
        }
        QTreeWidget::item:selected {
            background-color: #5C7C98;
            color: white;
        }
        QHeaderView::section {
            background-color: #EEF2F5;
            color: #3a4a5a;
            padding: 7px 8px;
            border: none;
            border-right: 1px solid #E0E6EC;
            border-bottom: 1px solid #D6DEE6;
            font-weight: 600;
        }
        QListWidget#TesterNav {
            background-color: #EEF2F5;
            border: none;
            border-right: 1px solid #D6DEE6;
            border-top-left-radius: 10px;
            border-bottom-left-radius: 10px;
            outline: none;
            font-size: 13px;
        }
        QListWidget#TesterNav::item {
            padding: 11px 16px;
            color: #3a4a5a;
            border-bottom: 1px solid #E4E9EE;
        }
        QListWidget#TesterNav::item:hover {
            background-color: rgba(92, 124, 152, 0.12);
        }
        QListWidget#TesterNav::item:selected {
            background-color: #5C7C98;
            color: white;
            font-weight: 600;
        }
        QStackedWidget#TesterStack {
            background: transparent;
        }
        QFrame#TesterCard {
            background-color: #FFFFFF;
            border: 1px solid #D8DEE4;
            border-radius: 10px;
        }
        QLabel#TesterCardTitle {
            font-size: 14px;
            font-weight: 600;
            color: #222933;
            background: transparent;
            border: none;
        }
        QProgressBar {
            border: none;
            border-radius: 5px;
            background-color: #E3E9EE;
            height: 8px;
            text-align: center;
        }
        QProgressBar::chunk {
            background-color: #5C7C98;
            border-radius: 5px;
        }
        QStatusBar {
            background-color: #5C7C98;
            color: white;
        }
        QStatusBar::item { border: none; }
        QComboBox {
            border: 1px solid #D6DEE6;
            border-radius: 8px;
            padding: 7px 12px;
            background-color: white;
            color: black;
        }
        QComboBox:hover {
            border-color: #5C7C98;
        }
        QLabel#VersionLabel {
            color: #7B8C9C;
            font-size: 11px;
            font-weight: 600;
            padding-top: 2px;
        }
        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 26px;
            border: none;
        }
        QComboBox::down-arrow {
            image: none;
            width: 0; height: 0;
            border-left: 5px solid transparent;
            border-right: 5px solid transparent;
            border-top: 6px solid #5C7C98;
            margin-right: 9px;
        }
        QComboBox QAbstractItemView {
            border: 1px solid #D6DEE6;
            background: white;
            color: black;
            selection-background-color: #5C7C98;
            selection-color: white;
            outline: none;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 11px;
            margin: 2px;
        }
        QScrollBar::handle:vertical {
            background: #C2CDD8;
            border-radius: 5px;
            min-height: 28px;
        }
        QScrollBar::handle:vertical:hover {
            background: #A7B7C6;
        }
        QScrollBar:horizontal {
            background: transparent;
            height: 11px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal {
            background: #C2CDD8;
            border-radius: 5px;
            min-width: 28px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #A7B7C6;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            width: 0; height: 0;
        }
        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
        }
        QToolTip {
            background-color: #2a3a4a;
            color: #ffffff;
            border: none;
            padding: 6px 9px;
            border-radius: 6px;
        }
    )").arg(bgStyle);

    setStyleSheet(qss);
}
