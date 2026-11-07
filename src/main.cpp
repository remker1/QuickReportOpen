// =============================================================================
// main.cpp - application entry point for QuickReports.
//
// Start-up sequence:
//   1. Install the crash/diagnostic logger (CrashHandler).
//   2. Create the QApplication and force the light colour scheme.
//   3. Register metatypes needed for queued cross-thread signals.
//   4. Make sure we are running elevated (relaunch via UAC "runas" if not).
//   5. Show a rounded splash screen while MainWindow is constructed.
//   6. Show the main window and run the Qt event loop.
// =============================================================================
#include <QApplication>
#include <QSplashScreen>
#include <QTimer>
#include <QDir>
#include <QThread>
#include <QPainter>
#include <QPainterPath>
#include <QIcon>
#include <QScreen>
#include <QStyleHints>
#include "MainWindow.h"
#include "CrashHandler.h"
#include "Version.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

// Returns true when the current process token is elevated (running "as
// administrator"). Disk management and fingerprint features need this.
// On non-Windows platforms it simply returns true.
bool isUserAdmin() {
#ifdef Q_OS_WIN
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    // Open the token of the current process for querying.
    if( OpenProcessToken( GetCurrentProcess( ),TOKEN_QUERY,&hToken ) ) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof( TOKEN_ELEVATION );
        // TokenElevation tells us whether the token is a full (elevated) one.
        if( GetTokenInformation( hToken, TokenElevation, &Elevation, sizeof( Elevation ), &cbSize ) ) {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if( hToken ) {
        CloseHandle( hToken );
    }
    return fRet;
#else
    return true; // Assume admin on non-Windows for simplicity or handle via sudo
#endif
}

// Starts a second copy of this executable with the "runas" verb, which makes
// Windows show the UAC consent dialog. The caller is expected to exit
// afterwards so only the elevated copy keeps running.
void relaunchAsAdmin() {
#ifdef Q_OS_WIN
    wchar_t szPath[MAX_PATH];
    if (GetModuleFileNameW(NULL, szPath, ARRAYSIZE(szPath))) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.lpVerb = L"runas";   // request elevation
        sei.lpFile = szPath;     // this very executable
        sei.hwnd = NULL;
        sei.nShow = SW_NORMAL;

        if (!ShellExecuteExW(&sei)) {
            // User refused the elevation
        }
    }
#endif
}

int main(int argc, char *argv[]) {
    // Initialize Crash Handler (truncates app.log, hooks Qt messages, signals
    // and std::terminate) before anything else can fail.
    CrashHandler::init();
    CrashHandler::log("Application started");

    QApplication app(argc, argv);

    // The app ships a light-only stylesheet. Without this, Windows dark mode
    // turns the default palette text white, which disappears against the
    // forced light backgrounds (any label without an explicit color).
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);

    // Register custom types used in queued (cross-thread) signals so they aren't
    // silently dropped (e.g. the deferred display update).
    qRegisterMetaType<QList<DisplayEntry>>("QList<DisplayEntry>");
    qRegisterMetaType<QList<StorageDisk>>("QList<StorageDisk>");
    qRegisterMetaType<QList<BatteryInfo>>("QList<BatteryInfo>");
    qRegisterMetaType<SystemSummaryData>("SystemSummaryData");

    // Set Application Icon
    app.setWindowIcon(QIcon(":/Logo1.png"));

    // 1. Admin Check
    // If not elevated, spawn an elevated copy and quit this (non-elevated) one.
    if (!isUserAdmin()) {
        CrashHandler::log("Requesting Admin privileges...");
        relaunchAsAdmin();
        return 0;
    }

    app.setApplicationName("QuickReports");
    app.setApplicationVersion(APP_VERSION);

    // 2. Splash Screen
    QPixmap pixmap(":/Splash1.png");
    QSplashScreen *splash = nullptr;

    if (!pixmap.isNull()) {
        // Scale to a fixed width of 600px
        pixmap = pixmap.scaledToWidth(600, Qt::SmoothTransformation);

        // Apply rounded corners: paint the image through a rounded-rect clip
        // path onto a transparent pixmap of the same size.
        QPixmap roundedPixmap(pixmap.size());
        roundedPixmap.fill(Qt::transparent);
        QPainter painter(&roundedPixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(0, 0, pixmap.width(), pixmap.height(), 20, 20);
        painter.setClipPath(path);
        painter.drawPixmap(0, 0, pixmap);
        painter.end();

        splash = new QSplashScreen(roundedPixmap);
        splash->show();
        app.processEvents();   // paint the splash before the heavy constructor runs
    }

    // 3. Main Window
    // Show the window as soon as it is constructed so it opens instantly even on
    // slow machines; the hardware scan continues in the background (the progress
    // bar reflects it). The splash is dismissed right away.
    MainWindow window(splash);
    window.show();
    if (splash) {
        splash->finish(&window);   // closes the splash once the window is visible
    }

    // Enter the Qt event loop; returns when the last window closes.
    int ret = app.exec();
    CrashHandler::log("Application exited normally");
    return ret;
}
