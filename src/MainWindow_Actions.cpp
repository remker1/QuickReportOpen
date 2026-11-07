// System / utility action handlers for MainWindow, split out of MainWindow.cpp
// to keep that file focused on window setup, scanning, and the summary UI.
// Covers: Install Softwares, Windows Update, time sync, Office
// folder, Word account, and device-encryption actions.
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
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <memory>
#include <algorithm>
#include <thread>
#include <vector>
#include <string>
#include "CrashHandler.h"
#include "CloneProgressDialog.h"
#include "Translations.h"
#include "hardwaretester/TesterTranslations.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

// Strategy order (stops at the first hit):
//   1. App Paths registry key (32/64-bit views)
//   2. Click-to-Run configuration InstallationPath
//   3. COM registration Word.Application -> CLSID -> LocalServer32
//   4. Brute-force scan of the standard Office install folders (Office16..Office11)
// ---- Local helpers (used only by the Word-account action) ----
// Returns the full path of WINWORD.EXE, or an empty string if it cannot be found.
static QString findWordPath() {
    const char *keys[] = {
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\winword.exe",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\App Paths\\winword.exe"
    };
    for (const char *k : keys) {
        QSettings s(k, QSettings::NativeFormat);
        QString p = s.value(".").toString();          // default (unnamed) value
        if (p.isEmpty()) p = s.value("Default").toString();
        p.remove('"');
        if (!p.isEmpty() && QFile::exists(p)) return p;
    }

    // Click-to-Run installation path (Office 365 / 2016-2021), e.g.
    // C:\Program Files\Microsoft Office\root\Office16\WINWORD.EXE
    for (const char *k : {"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Office\\ClickToRun\\Configuration",
                          "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Office\\ClickToRun\\Configuration"}) {
        QSettings s(k, QSettings::NativeFormat);
        const QString root = s.value("InstallationPath").toString();
        if (!root.isEmpty()) {
            for (const QString &sub : {QStringLiteral("/root/Office16/WINWORD.EXE"),
                                       QStringLiteral("/Office16/WINWORD.EXE")}) {
                const QString p = root + sub;
                if (QFile::exists(p)) return p;
            }
        }
    }

    // COM registration: Word.Application -> CLSID -> LocalServer32. This is the
    // reliable source when App Paths is absent (some Click-to-Run/Store installs),
    // since the ProgID is always registered when Word is installed.
    {
        QString clsid;
        QSettings progid("HKEY_CLASSES_ROOT\\Word.Application\\CLSID", QSettings::NativeFormat);
        clsid = progid.value(".").toString();
        if (clsid.isEmpty()) {
            QSettings curver("HKEY_CLASSES_ROOT\\Word.Application\\CurVer", QSettings::NativeFormat);
            const QString cur = curver.value(".").toString();
            if (!cur.isEmpty()) {
                QSettings p2(QString("HKEY_CLASSES_ROOT\\%1\\CLSID").arg(cur), QSettings::NativeFormat);
                clsid = p2.value(".").toString();
            }
        }
        if (!clsid.isEmpty()) {
            QSettings ls(QString("HKEY_CLASSES_ROOT\\CLSID\\%1\\LocalServer32").arg(clsid),
                         QSettings::NativeFormat);
            QString cmd = ls.value(".").toString();
            if (cmd.isEmpty()) cmd = ls.value("Default").toString();
            cmd = cmd.trimmed();
            // Value looks like: "C:\...\WINWORD.EXE" /automation — take the exe.
            if (cmd.startsWith('"')) {
                const int end = cmd.indexOf('"', 1);
                if (end > 0) cmd = cmd.mid(1, end - 1);
            } else {
                const int sw = cmd.indexOf(" /");
                if (sw > 0) cmd = cmd.left(sw);
            }
            cmd.remove('"');
            if (!cmd.isEmpty() && QFile::exists(cmd)) return cmd;
        }
    }

    // Last resort: brute-force the well-known install directories.
    QStringList roots;
    for (const char *env : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        const QString base = QString::fromLocal8Bit(qgetenv(env));
        if (!base.isEmpty()) {
            roots << base + "/Microsoft Office/root";
            roots << base + "/Microsoft Office";
        }
    }
    for (const QString &root : roots) {
        for (int ver = 16; ver >= 11; --ver) {
            const QString p = QString("%1/Office%2/WINWORD.EXE").arg(root).arg(ver);
            if (QFile::exists(p)) return p;
        }
    }
    return QString();
}

// True when Word can be automated via COM (New-Object Word.Application). The
// ProgID is registered whenever Word is installed, even when App Paths and the
// install-dir scan miss it — so this is the truest gate for the account button,
// which opens Word through COM.
static bool isWordAvailable() {
    if (!findWordPath().isEmpty())
        return true;
    for (const char *k : {"HKEY_CLASSES_ROOT\\Word.Application\\CLSID",
                          "HKEY_CLASSES_ROOT\\Word.Application\\CurVer"}) {
        QSettings s(k, QSettings::NativeFormat);
        if (!s.value(".").toString().isEmpty())
            return true;
    }
    return false;
}

// ---- Action handlers ----
// "Install Softwares": lists installers (*.iso, *.img, *.exe) found in the
// "Softwares" folder next to the launcher, lets the user tick several, and starts
// each one (images are mounted and Setup.exe run; exes start detached).
void MainWindow::onInstallSoftwaresClicked() {
    // Resolve the folder that holds the installers: a "Softwares" folder next to
    // the launcher (one level up when we are running from QuickReportsDist).
    QDir dir(HardwareModel::getBaseDir());
    if (dir.dirName().compare("QuickReportsDist", Qt::CaseInsensitive) == 0) {
        dir.cdUp();
    }
    QDir softwaresDir(dir.filePath("Softwares"));
    if (!softwaresDir.exists()) {
        QMessageBox::warning(this, getTranslation("install_softwares"),
                             getTranslation("softwares_folder_not_found"));
        return;
    }

    // Collect the installers: .iso/.img images (mounted + Setup.exe) and .exe
    // files (run directly). The Office installation image is picked up here like
    // any other disk image and still installs through the original
    // mount-and-run path (OfficeSetup.img continues to work).
    QStringList filters;
    filters << "*.iso" << "*.img" << "*.exe";
    QFileInfoList installers = softwaresDir.entryInfoList(
        filters, QDir::Files, QDir::Name | QDir::IgnoreCase);
    if (installers.isEmpty()) {
        QMessageBox::warning(this, getTranslation("install_softwares"),
                             getTranslation("softwares_none_found"));
        return;
    }

    // Multi-select picker: one checkable row per installer.
    QDialog dialog(this);
    dialog.setWindowTitle(getTranslation("install_softwares_select_title"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(getTranslation("install_softwares_select_prompt"), &dialog));

    QListWidget *list = new QListWidget(&dialog);
    for (const QFileInfo &fi : installers) {
        QListWidgetItem *item = new QListWidgetItem(fi.fileName(), list);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
        item->setData(Qt::UserRole, fi.absoluteFilePath());
    }
    layout->addWidget(list);

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    QList<QFileInfo> selected;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->checkState() == Qt::Checked) {
            selected.append(QFileInfo(item->data(Qt::UserRole).toString()));
        }
    }
    if (selected.isEmpty()) {
        return;
    }

    // Launch each selected installer. Installers are interactive, so they run
    // detached (.exe) or mounted-and-detached (.iso) rather than blocking.
    for (const QFileInfo &fi : selected) {
        const QString suffix = fi.suffix().toLower();
        if (suffix == "iso" || suffix == "img") {
            installIsoImage(fi.absoluteFilePath());
        } else {
            appendLog("Launching " + fi.fileName() + "...");
            QProcess::startDetached(fi.absoluteFilePath(), QStringList(),
                                    fi.absolutePath());
        }
    }
}

// Mounts an .iso/.img with Mount-DiskImage, runs Setup.exe from the new drive root
// (or opens the drive if there is none) and remembers the image so it can be
// dismounted when the application closes.
void MainWindow::installIsoImage(const QString &imagePath) {
    // Original "run office setup" behaviour, generalised to any disk image
    // (.iso or .img): mount the image, run its Setup.exe from the mounted drive
    // root, and remember the path so it is dismounted when the app closes.
    // Mount-DiskImage handles both formats identically.
    if (imagePath.isEmpty() || !QFile::exists(imagePath)) {
        QMessageBox::warning(this, getTranslation("install_softwares"),
                             getTranslation("softwares_none_found"));
        return;
    }

    appendLog("Mounting " + imagePath + "...");
    QProcess mountProc;
    // Mount-DiskImage -ImagePath "..." -PassThru | Get-Volume | Select-Object -ExpandProperty DriveLetter
    QString psCmd = QString("Mount-DiskImage -ImagePath \"%1\" -PassThru | Get-Volume | Select-Object -ExpandProperty DriveLetter").arg(imagePath);
    mountProc.start("powershell", QStringList() << "-Command" << psCmd);
    mountProc.waitForFinished();

    QString output = mountProc.readAllStandardOutput().trimmed();
    if (output.isEmpty()) {
        QMessageBox::critical(this, getTranslation("error_title"), "Failed to mount disk image or get drive letter.");
        return;
    }
    mountedImagePaths.append(imagePath);

    // Output might contain multiple lines if the image exposes multiple volumes;
    // the installer volume is normally the last one.
    QString driveLetter = output.split("\r\n").last().trimmed();

    QString setupPath = driveLetter + ":\\Setup.exe";
    appendLog("Looking for " + setupPath);

    if (QFile::exists(setupPath)) {
        // Run Setup.exe with the mounted drive root as its working directory.
        QProcess::startDetached(setupPath, QStringList(), driveLetter + ":\\");
    } else {
        // Setup.exe not at the root — open the drive so the user can find it.
        QDesktopServices::openUrl(QUrl::fromLocalFile(driveLetter + ":\\"));
        QMessageBox::warning(this, getTranslation("install_softwares"), "Setup.exe not found on mounted drive. Opened drive folder.");
    }
}

// Dismounts every disk image mounted by installIsoImage().
void MainWindow::dismountOfficeImage() {
    for (const QString &imgPath : mountedImagePaths) {
        QString ps = QString("Dismount-DiskImage -ImagePath \"%1\"").arg(imgPath);
        QProcess::execute("powershell", QStringList() << "-Command" << ps);
    }
    mountedImagePaths.clear();
}

// Window close: make sure installer images do not stay mounted.
void MainWindow::closeEvent(QCloseEvent *event) {
    dismountOfficeImage();
    QMainWindow::closeEvent(event);
}

// Opens the Windows Update page of the Settings app.
void MainWindow::onUpdatesClicked() {
    QDesktopServices::openUrl(QUrl("ms-settings:windowsupdate"));
}

// Background check (PowerShell + Windows Update Agent COM API) that counts pending
// required vs optional updates; the result is shown as a badge by the Windows
// Update button (see onUpdateCheckFinished). Skipped if a check is already running.
void MainWindow::checkForUpdates() {
    #ifdef Q_OS_WIN
    if (updateCheckProc && updateCheckProc->state() != QProcess::NotRunning)
        return;

    // Query the Windows Update Agent for not-installed updates, split into
    // required vs optional. Optional updates (incl. most driver updates) have
    // BrowseOnly = true.
    QString ps = R"PS(
$ErrorActionPreference = 'Stop'
try {
    $session  = New-Object -ComObject Microsoft.Update.Session
    $searcher = $session.CreateUpdateSearcher()
    $result   = $searcher.Search("IsInstalled=0")
    $req = 0; $opt = 0
    foreach ($u in $result.Updates) {
        if ($u.BrowseOnly) { $opt++ } else { $req++ }
    }
    Write-Output ("REQ:" + $req + ";OPT:" + $opt)
} catch {
    Write-Output "ERR"
}
)PS";

    QString scriptPath = QDir::temp().filePath("check_windows_updates.ps1");
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    {
        QTextStream out(&script);
        out << ps;
    }
    script.close();

    if (!updateCheckProc) {
        updateCheckProc = new QProcess(this);
        connect(updateCheckProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, &MainWindow::onUpdateCheckFinished);
    }
    updateCheckScriptPath = scriptPath;
    updateCheckProc->start("powershell", QStringList()
        << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-File" << scriptPath);
    #endif
}

// Parses "REQ:n;OPT:m" from the script output and colours the badge:
// red = required updates, yellow = only optional, green check = up to date.
// On any parse failure the badge is hidden rather than showing wrong info.
void MainWindow::onUpdateCheckFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(exitCode);
    Q_UNUSED(status);

    QString output = QString::fromLocal8Bit(updateCheckProc->readAllStandardOutput());
    if (!updateCheckScriptPath.isEmpty())
        QFile::remove(updateCheckScriptPath);

    // Could not determine status — hide the badge rather than mislead.
    QRegularExpression re("REQ:(\\d+);OPT:(\\d+)");
    auto m = re.match(output);
    if (!m.hasMatch()) {
        updateBadge->hide();
        return;
    }

    int required = m.captured(1).toInt();
    int optional = m.captured(2).toInt();

    QString bg, fg = "white", text, tip;
    if (required > 0) {
        bg   = "#C0392B"; // red: important updates pending
        text = (required > 99) ? "99+" : QString::number(required);
        tip  = tr("%1 important update(s) available").arg(required);
        if (optional > 0)
            tip += tr(" (+%1 optional)").arg(optional);
    } else if (optional > 0) {
        bg   = "#F1C40F"; // yellow: only optional updates
        fg   = "#5a4a00";  // dark text for contrast on yellow
        text = (optional > 99) ? "99+" : QString::number(optional);
        tip  = tr("%1 optional update(s) available").arg(optional);
    } else {
        bg   = "#27AE60"; // green: up to date
        text = "✓";
        tip  = tr("No updates available");
    }

    updateBadge->setStyleSheet(
        QString("background:%1; color:%2; border-radius:10px;"
                " padding:0 6px; font-weight:bold; font-size:11px;").arg(bg, fg));
    updateBadge->setText(text);
    updateBadge->setToolTip(tip);
    updateBadge->show();
}

// Turns on the per-user "Let apps access your location" switch from this
// (interactive-user) process. The elevated PowerShell below does the same, but
// when UAC elevates as a *different* admin account its HKCU is that account's,
// not the logged-in user's — so set the real user's value here as well.
// (see comment above) helper shared by the two time-fix actions.
static void allowAppLocationForCurrentUser() {
    #ifdef Q_OS_WIN
    QSettings loc("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\"
                  "CapabilityAccessManager\\ConsentStore\\location", QSettings::NativeFormat);
    loc.setValue("Value", "Allow");
    loc.sync();
    #endif
}

// "Auto Time Zone & Sync": runs an elevated PowerShell one-liner that enables
// Location, the automatic time-zone service and the Windows Time service, then
// forces a resync.
void MainWindow::onSyncTimeClicked() {
    #ifdef Q_OS_WIN
    allowAppLocationForCurrentUser();
    appendLog(getTranslation("sync_time_progress"));

    // These operations require administrator privileges. We run a single elevated
    // PowerShell command that:
    //   1. Turns Location Services on when they are off — Windows' automatic
    //      time zone (tzautoupdate) silently does nothing without location.
    //   2. Sets the time zone auto-update service (tzautoupdate) to automatic and starts it.
    //   3. Ensures the Windows Time service (w32time) is running.
    //   4. Forces an immediate time resync.
    // Only single quotes inside: the whole script is passed via -Command "...".
    QString psScript =
        // 1. Location — clear any blocking policy, allow the capability, start the service.
        "$pol='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors'; "
        "if (Test-Path $pol) { Remove-ItemProperty -Path $pol -Name DisableLocation -ErrorAction SilentlyContinue }; "
        "$loc='HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location'; "
        "if (-not (Test-Path $loc)) { New-Item -Path $loc -Force | Out-Null }; "
        "$cur=(Get-ItemProperty -Path $loc -Name Value -ErrorAction SilentlyContinue).Value; "
        "if ($cur -ne 'Allow') { Set-ItemProperty -Path $loc -Name Value -Value 'Allow'; "
        "Write-Host 'Location was OFF - turned ON for automatic time zone.' } "
        "else { Write-Host 'Location already ON.' }; "
        // The Settings page has TWO switches: "Location services" (HKLM, above) and
        // "Let apps access your location" (per-user, HKCU). Time zone auto-update
        // needs both — with only the first on it reports location ON but never
        // changes the time zone. Also clear the app-privacy policy that forces it off.
        "$app='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy'; "
        "if (Test-Path $app) { Remove-ItemProperty -Path $app -Name LetAppsAccessLocation* -ErrorAction SilentlyContinue }; "
        "$usr='HKCU:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location'; "
        "if (-not (Test-Path $usr)) { New-Item -Path $usr -Force | Out-Null }; "
        "Set-ItemProperty -Path $usr -Name Value -Value 'Allow'; "
        "Write-Host 'App location access (current user) is ON.'; "
        "Set-Service -Name lfsvc -StartupType Automatic -ErrorAction SilentlyContinue; "
        "Start-Service -Name lfsvc -ErrorAction SilentlyContinue; "
        // 2-4. Automatic time zone, Windows Time service, immediate resync.
        "Set-Service -Name tzautoupdate -StartupType Automatic; "
        "Start-Service -Name tzautoupdate -ErrorAction SilentlyContinue; "
        "Set-Service -Name w32time -StartupType Automatic; "
        "Start-Service -Name w32time -ErrorAction SilentlyContinue; "
        "w32tm /resync /force; "
        "Write-Host 'Time zone set to automatic and time synced.'; "
        "Start-Sleep -Seconds 3";

    QString params = "-NoProfile -ExecutionPolicy Bypass -Command \"" + psScript + "\"";

    HINSTANCE result = ShellExecuteW(nullptr, L"runas", L"powershell.exe",
                                     (LPCWSTR)params.toStdWString().c_str(),
                                     nullptr, SW_SHOWNORMAL);

    if ((INT_PTR)result <= 32) {
        appendLog(getTranslation("sync_time_failed"));
        QMessageBox::warning(this, getTranslation("sync_time"), getTranslation("sync_time_failed"));
    } else {
        appendLog(getTranslation("sync_time_launched"));
    }
    #endif
}

// Troubleshoot variant of the above that also switches the NTP server to
// time.google.com (useful when time.windows.com is unreachable).
void MainWindow::onFixTimeServerClicked() {
    #ifdef Q_OS_WIN
    allowAppLocationForCurrentUser();
    appendLog(getTranslation("fix_time_progress"));

    // Troubleshoot fallback for when the plain "Auto Time Zone & Sync" doesn't
    // fix the clock — usually because the default NTP peer (time.windows.com)
    // is unreachable. This elevated PowerShell command:
    //   1. Turns Location Services on when off (automatic time zone needs it).
    //   2. Points w32time at Google's NTP server (time.google.com) and also
    //      registers Google as the default entry in the Date & Time server list.
    //   3. Re-enables the automatic time zone service (tzautoupdate).
    //   4. Restarts the Windows Time service and forces an immediate resync.
    // Only single quotes inside: the whole script is passed via -Command "...".
    QString psScript =
        // 1. Location — clear any blocking policy, allow the capability, start the service.
        "$pol='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\LocationAndSensors'; "
        "if (Test-Path $pol) { Remove-ItemProperty -Path $pol -Name DisableLocation -ErrorAction SilentlyContinue }; "
        "$loc='HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location'; "
        "if (-not (Test-Path $loc)) { New-Item -Path $loc -Force | Out-Null }; "
        "$cur=(Get-ItemProperty -Path $loc -Name Value -ErrorAction SilentlyContinue).Value; "
        "if ($cur -ne 'Allow') { Set-ItemProperty -Path $loc -Name Value -Value 'Allow'; "
        "Write-Host 'Location was OFF - turned ON for automatic time zone.' } "
        "else { Write-Host 'Location already ON.' }; "
        // The Settings page has TWO switches: "Location services" (HKLM, above) and
        // "Let apps access your location" (per-user, HKCU). Time zone auto-update
        // needs both — with only the first on it reports location ON but never
        // changes the time zone. Also clear the app-privacy policy that forces it off.
        "$app='HKLM:\\SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy'; "
        "if (Test-Path $app) { Remove-ItemProperty -Path $app -Name LetAppsAccessLocation* -ErrorAction SilentlyContinue }; "
        "$usr='HKCU:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location'; "
        "if (-not (Test-Path $usr)) { New-Item -Path $usr -Force | Out-Null }; "
        "Set-ItemProperty -Path $usr -Name Value -Value 'Allow'; "
        "Write-Host 'App location access (current user) is ON.'; "
        "Set-Service -Name lfsvc -StartupType Automatic -ErrorAction SilentlyContinue; "
        "Start-Service -Name lfsvc -ErrorAction SilentlyContinue; "
        // 2. Register Google in the Date & Time server list and make it the default (index 1).
        "$srv='HKLM:\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DateTime\\Servers'; "
        "if (-not (Test-Path $srv)) { New-Item -Path $srv -Force | Out-Null }; "
        "Set-ItemProperty -Path $srv -Name '1' -Value 'time.google.com'; "
        "Set-ItemProperty -Path $srv -Name '(Default)' -Value '1'; "
        // 2b. Point w32time at Google's NTP server.
        "w32tm /config /manualpeerlist:'time.google.com,0x9' /syncfromflags:manual /reliable:yes /update; "
        // 3. Automatic time zone service.
        "Set-Service -Name tzautoupdate -StartupType Automatic; "
        "Start-Service -Name tzautoupdate -ErrorAction SilentlyContinue; "
        // 4. Restart Windows Time and force an immediate resync from Google.
        "Set-Service -Name w32time -StartupType Automatic; "
        "Restart-Service -Name w32time -ErrorAction SilentlyContinue; "
        "w32tm /resync /force; "
        "Write-Host 'Time server set to Google, time zone automatic and time synced.'; "
        "Start-Sleep -Seconds 3";

    QString params = "-NoProfile -ExecutionPolicy Bypass -Command \"" + psScript + "\"";

    HINSTANCE result = ShellExecuteW(nullptr, L"runas", L"powershell.exe",
                                     (LPCWSTR)params.toStdWString().c_str(),
                                     nullptr, SW_SHOWNORMAL);

    if ((INT_PTR)result <= 32) {
        appendLog(getTranslation("fix_time_failed"));
        QMessageBox::warning(this, getTranslation("fix_time_title"), getTranslation("fix_time_failed"));
    } else {
        appendLog(getTranslation("fix_time_launched"));
    }
    #endif
}

// True when a Windows service with exactly this display name exists.
// (PowerShell Get-Service lookup, 15 s timeout, no console window.)
static bool serviceExistsByDisplayName(const QString &displayName) {
    #ifdef Q_OS_WIN
    QProcess ps;
    ps.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW;
    });
    const QString cmd = QString("if (Get-Service | Where-Object { $_.DisplayName -eq '%1' }) "
                                "{ exit 0 } else { exit 1 }").arg(displayName);
    ps.start("powershell.exe", {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", cmd});
    if (!ps.waitForFinished(15000)) {
        ps.kill();
        return false;
    }
    return ps.exitStatus() == QProcess::NormalExit && ps.exitCode() == 0;
    #else
    Q_UNUSED(displayName);
    return false;
    #endif
}

// Downloads url to destPath behind a modal progress dialog (follows redirects).
// Writes to a .part file and renames on success; returns false on any failure
// or cancel. Basic sanity check: a Windows executable must start with "MZ".
// (see comment above)
bool MainWindow::downloadFile(const QUrl &url, const QString &destPath) {
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = nam.get(req);

    const QString partPath = destPath + ".part";
    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly)) {
        reply->deleteLater();
        return false;
    }

    QProgressDialog dlg(getTranslation("hp_hotkey_download_progress"), "Cancel", 0, 100, this);
    dlg.setWindowTitle(getTranslation("hp_hotkey_title"));
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setMinimumDuration(0);
    dlg.setValue(0);

    QEventLoop loop;
    connect(reply, &QNetworkReply::readyRead, &loop, [&]() { part.write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 got, qint64 total) {
        if (total > 0) dlg.setValue(int(got * 100 / total));
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&dlg, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    loop.exec();

    part.write(reply->readAll());
    part.close();
    const bool ok = reply->error() == QNetworkReply::NoError && !dlg.wasCanceled();
    reply->deleteLater();
    dlg.close();

    bool valid = false;
    if (ok) {
        QFile f(partPath);
        valid = f.size() > 100 * 1024 && f.open(QIODevice::ReadOnly) && f.read(2) == "MZ";
    }
    if (!valid) {
        QFile::remove(partPath);
        return false;
    }
    QFile::remove(destPath);
    return QFile::rename(partPath, destPath);
}

// HP Hotkey fix, two phases (see header): if "HP Hotkey Service" is missing,
// download (after consent) and run the HP SoftPaq installer; once it exists,
// disable the legacy "HP Hotkey UWP Service" via an elevated script, wait for it
// by polling the process handle, and offer a restart.
void MainWindow::onHpHotkeyFixClicked() {
    #ifdef Q_OS_WIN
    // Phase 2: the CMIT driver's "HP Hotkey Service" exists, so the driver is in
    // place — disable the legacy UWP service that pops up on every key press.
    if (serviceExistsByDisplayName("HP Hotkey Service")) {
        appendLog(getTranslation("hp_hotkey_fix_progress"));

        // Only single quotes inside: the script is passed via -Command "...".
        const QString psScript =
            "$svcs = Get-Service | Where-Object { $_.DisplayName -eq 'HP Hotkey UWP Service' }; "
            "if (-not $svcs) { Write-Host 'HP Hotkey UWP Service not found - nothing to disable.' } "
            "foreach ($s in $svcs) { "
            "Stop-Service -Name $s.Name -Force -ErrorAction SilentlyContinue; "
            "Set-Service -Name $s.Name -StartupType Disabled; "
            "Write-Host ('Stopped and disabled: ' + $s.DisplayName) }; "
            "Write-Host 'Restart the laptop to apply the changes.'; "
            "Start-Sleep -Seconds 4";
        const QString params = "-NoProfile -ExecutionPolicy Bypass -Command \"" + psScript + "\"";

        // Run elevated and wait for it to finish so we can offer the restart.
        const std::wstring wparams = params.toStdWString();
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"powershell.exe";
        sei.lpParameters = wparams.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            appendLog(getTranslation("hp_hotkey_fix_failed"));
            QMessageBox::warning(this, getTranslation("hp_hotkey_title"),
                                 getTranslation("hp_hotkey_fix_failed"));
            return;
        }
        hpHotkeyBtn->setEnabled(false);
        HANDLE proc = sei.hProcess;
        QTimer *poll = new QTimer(this);
        connect(poll, &QTimer::timeout, this, [this, poll, proc]() {
            if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT)
                return;
            DWORD code = 1;
            GetExitCodeProcess(proc, &code);
            CloseHandle(proc);
            poll->stop();
            poll->deleteLater();
            hpHotkeyBtn->setEnabled(true);
            if (code != 0) {
                appendLog(getTranslation("hp_hotkey_fix_failed"));
                QMessageBox::warning(this, getTranslation("hp_hotkey_title"),
                                     getTranslation("hp_hotkey_fix_failed"));
                return;
            }
            appendLog(getTranslation("hp_hotkey_fix_launched"));
            if (QMessageBox::question(this, getTranslation("hp_hotkey_title"),
                                      getTranslation("hp_hotkey_restart_prompt"))
                == QMessageBox::Yes) {
                QProcess::startDetached("shutdown.exe", {"/r", "/t", "5"});
            }
        });
        poll->start(500);
        return;
    }

    // Phase 1: driver missing — run Drivers\sp91903.exe (next to the launcher,
    // one level up when running from QuickReportsDist), then a reboot is needed.
    QDir dir(HardwareModel::getBaseDir());
    if (dir.dirName().compare("QuickReportsDist", Qt::CaseInsensitive) == 0) {
        dir.cdUp();
    }
    const QString installer = dir.filePath("Drivers/sp91903.exe");
    if (!QFile::exists(installer)) {
        // Create the Drivers folder if needed, then offer to download the driver.
        if (!dir.exists("Drivers"))
            dir.mkdir("Drivers");
        if (QMessageBox::question(this, getTranslation("hp_hotkey_title"),
                                  getTranslation("hp_hotkey_download_confirm"))
            != QMessageBox::Yes) {
            return;
        }
        appendLog(getTranslation("hp_hotkey_download_progress"));
        if (!downloadFile(QUrl("https://ftp.hp.com/pub/softpaq/sp91501-92000/sp91903.exe"),
                          installer)) {
            appendLog(getTranslation("hp_hotkey_download_failed"));
            QMessageBox::warning(this, getTranslation("hp_hotkey_title"),
                                 getTranslation("hp_hotkey_download_failed"));
            return;
        }
        appendLog(getTranslation("hp_hotkey_download_done"));
    }
    if (QMessageBox::question(this, getTranslation("hp_hotkey_title"),
                              getTranslation("hp_hotkey_install_confirm"))
        != QMessageBox::Yes) {
        return;
    }

    const QString native = QDir::toNativeSeparators(installer);
    const QString workDir = QDir::toNativeSeparators(QFileInfo(installer).absolutePath());
    HINSTANCE result = ShellExecuteW(nullptr, L"runas", (LPCWSTR)native.toStdWString().c_str(),
                                     nullptr, (LPCWSTR)workDir.toStdWString().c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)result <= 32) {
        appendLog(getTranslation("hp_hotkey_install_failed"));
        QMessageBox::warning(this, getTranslation("hp_hotkey_title"),
                             getTranslation("hp_hotkey_install_failed"));
    } else {
        appendLog(getTranslation("hp_hotkey_install_launched"));
    }
    #endif
}

// Installs Lenovo Commercial Vantage via `winget install` from the msstore
// source with a progress dialog (percentages parsed from winget's output). Falls
// back to opening the Microsoft Store product page if winget is unavailable or fails.
void MainWindow::onLenovoVantageClicked() {
    #ifdef Q_OS_WIN
    static const QString kProductId = QStringLiteral("9NR5B8GVVM13"); // Lenovo Commercial Vantage

    auto openStorePage = [this]() {
        appendLog(getTranslation("lenovo_vantage_open_store"));
        // Opens the Store app directly on the product page; fall back to the web page.
        if (!QDesktopServices::openUrl(QUrl("ms-windows-store://pdp/?ProductId=" + kProductId))
            && !QDesktopServices::openUrl(QUrl("https://apps.microsoft.com/detail/" + kProductId))) {
            appendLog(getTranslation("lenovo_vantage_store_failed"));
            QMessageBox::warning(this, getTranslation("lenovo_vantage_title"),
                                 getTranslation("lenovo_vantage_store_failed"));
        }
    };

    // Direct install through winget's msstore source (no admin needed).
    const QString winget = QStandardPaths::findExecutable("winget");
    if (winget.isEmpty()) {
        openStorePage();
        return;
    }

    appendLog(getTranslation("lenovo_vantage_installing"));
    lenovoVantageBtn->setEnabled(false);

    // Modal progress bar: busy (indeterminate) until winget reports a download
    // percentage, then a real 0-100 bar; busy again once it starts installing.
    QProgressDialog *dlg = new QProgressDialog(getTranslation("lenovo_vantage_installing"),
                                               QString(), 0, 0, this);
    dlg->setWindowTitle(getTranslation("lenovo_vantage_title"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setWindowFlag(Qt::WindowCloseButtonHint, false);
    dlg->setCancelButton(nullptr);
    dlg->setAutoClose(false);
    dlg->setAutoReset(false);
    dlg->setMinimumDuration(0);
    dlg->setMinimumWidth(420);
    dlg->show();

    QProcess *proc = new QProcess(this);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *a) {
        a->flags |= CREATE_NO_WINDOW;
    });
    connect(proc, &QProcess::readyRead, this, [proc, dlg]() {
        // winget redraws its progress bar with '\r'; take the last percentage.
        const QString out = QString::fromUtf8(proc->readAll());
        static const QRegularExpression pct("(\\d{1,3})\\s*%");
        int last = -1;
        auto it = pct.globalMatch(out);
        while (it.hasNext())
            last = it.next().captured(1).toInt();
        if (out.contains("Starting package install", Qt::CaseInsensitive)) {
            dlg->setRange(0, 0);
        } else if (last >= 0 && last <= 100) {
            if (dlg->maximum() == 0)
                dlg->setRange(0, 100);
            dlg->setValue(last);
        }
    });
    auto finish = [this, proc, dlg]() {
        dlg->close();
        dlg->deleteLater();
        proc->deleteLater();
        lenovoVantageBtn->setEnabled(true);
    };
    connect(proc, &QProcess::finished, this,
            [this, finish, openStorePage](int exitCode, QProcess::ExitStatus status) {
        finish();
        // 0 = installed; 0x8A15002B = already installed / no newer version.
        if (status == QProcess::NormalExit &&
            (exitCode == 0 || static_cast<quint32>(exitCode) == 0x8A15002Bu)) {
            appendLog(getTranslation("lenovo_vantage_done"));
            QMessageBox::information(this, getTranslation("lenovo_vantage_title"),
                                     getTranslation("lenovo_vantage_done"));
        } else {
            openStorePage();
        }
    });
    connect(proc, &QProcess::errorOccurred, this,
            [finish, openStorePage](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        finish();
        openStorePage();
    });
    proc->start(winget, {"install", "--id", kProductId, "--source", "msstore",
                         "--accept-package-agreements", "--accept-source-agreements"});
    #endif
}

// Creates Desktop\Office with shortcuts to every installed Office app. The
// PowerShell script resolves each exe from App Paths (then a folder scan) and
// reports how many shortcuts it made via a "CREATED:n" line.
void MainWindow::onCreateOfficeFolderClicked() {
    #ifdef Q_OS_WIN
    appendLog(getTranslation("office_folder_progress"));

    // Write a PowerShell script that creates a Desktop\Office folder and populates it
    // with .lnk shortcuts to whichever Office apps are installed. No admin rights needed
    // since it only writes to the current user's Desktop.
    QString scriptPath = QDir::temp().filePath("create_office_shortcuts.ps1");
    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, getTranslation("office_folder"), getTranslation("office_folder_failed"));
        return;
    }

    QString ps = R"PS(
$ErrorActionPreference = 'SilentlyContinue'

$desktop = [Environment]::GetFolderPath('Desktop')
$folder  = Join-Path $desktop 'Office'
New-Item -ItemType Directory -Path $folder -Force | Out-Null

# Office executable -> friendly shortcut name
$apps = [ordered]@{
    'WINWORD.EXE'  = 'Word'
    'EXCEL.EXE'    = 'Excel'
    'POWERPNT.EXE' = 'PowerPoint'
    'OUTLOOK.EXE'  = 'Outlook'
    'ONENOTE.EXE'  = 'OneNote'
    'MSACCESS.EXE' = 'Access'
    'MSPUB.EXE'    = 'Publisher'
    'VISIO.EXE'    = 'Visio'
    'WINPROJ.EXE'  = 'Project'
}

# Primary lookup: the App Paths registry key, which every Office installer
# (2007 through 2024 / 365, 32- and 64-bit) registers for its executables.
function Resolve-OfficeExe([string]$exe) {
    $keys = @(
        "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$exe",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\$exe"
    )
    foreach ($k in $keys) {
        $item = Get-Item -Path $k -ErrorAction SilentlyContinue
        if ($item) {
            $val = $item.GetValue('')   # default value = full exe path
            if ($val) { $val = $val.Trim('"') }
            if ($val -and (Test-Path $val)) { return $val }
        }
    }

    # Fallback: scan known install roots, including version-named folders
    # (Office12=2007, 14=2010, 15=2013, 16=2016/2019/2021/365) and Click-to-Run.
    $roots = @(
        "$env:ProgramFiles\Microsoft Office",
        "${env:ProgramFiles(x86)}\Microsoft Office",
        "$env:ProgramFiles\Microsoft Office 15",
        "${env:ProgramFiles(x86)}\Microsoft Office 15"
    )
    foreach ($r in $roots) {
        if (Test-Path $r) {
            $f = Get-ChildItem -Path $r -Recurse -Filter $exe -ErrorAction SilentlyContinue |
                 Select-Object -First 1
            if ($f) { return $f.FullName }
        }
    }
    return $null
}

# Derive the release year from the file's major version where unambiguous.
function Get-OfficeYear([string]$path) {
    try {
        switch ((Get-Item $path).VersionInfo.FileMajorPart) {
            11 { return '2003' }
            12 { return '2007' }
            14 { return '2010' }
            15 { return '2013' }
            default { return '' }  # 16.x covers 2016/2019/2021/365 - year is ambiguous
        }
    } catch { return '' }
}

$shell = New-Object -ComObject WScript.Shell
$count = 0

foreach ($exe in $apps.Keys) {
    $path = Resolve-OfficeExe $exe
    if ($path -and (Test-Path $path)) {
        $name = $apps[$exe]
        $year = Get-OfficeYear $path
        if ($year) { $name = "$name $year" }
        $lnk = $shell.CreateShortcut((Join-Path $folder ($name + '.lnk')))
        $lnk.TargetPath = $path
        $lnk.Save()
        $count++
    }
}

# Open the folder so the user can see the result.
Start-Process explorer.exe $folder
Write-Output "CREATED:$count"
)PS";

    QTextStream out(&script);
    out << ps;
    script.close();

    QProcess proc;
    proc.start("powershell", QStringList()
               << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-File" << scriptPath);
    proc.waitForFinished(-1);

    QString output = proc.readAllStandardOutput();
    int count = 0;
    QRegularExpression re("CREATED:(\\d+)");
    auto m = re.match(output);
    if (m.hasMatch()) count = m.captured(1).toInt();

    QFile::remove(scriptPath);

    if (count > 0) {
        QString msg = getTranslation("office_folder_done").replace("{count}", QString::number(count));
        appendLog(msg);
        QMessageBox::information(this, getTranslation("office_folder"), msg);
    } else {
        appendLog(getTranslation("office_folder_none"));
        QMessageBox::warning(this, getTranslation("office_folder"), getTranslation("office_folder_none"));
    }
    #endif
}

// Opens Word on its File > Account page. Word is started directly (COM
// activation often fails when elevated) with a blank document, brought to the
// foreground, then UI Automation clicks "File" and "Account" by their localized
// names. A diagnostic log is written to %TEMP%\qr_word_account.log.
void MainWindow::onWordAccountClicked() {
    #ifdef Q_OS_WIN
    if (!isWordAvailable()) {
        QMessageBox::information(this, getTranslation("word_account"),
                                getTranslation("word_not_found"));
        return;
    }

    // Launch Word and navigate to the Account backstage page. We start WINWORD.EXE
    // directly rather than activating the out-of-process Word.Application COM
    // server: this app runs elevated, and out-of-proc COM activation of Office
    // often fails from an elevated process (leaving nothing on screen). A blank
    // document is opened so the ribbon is present, then the File → Account items
    // are clicked via UI Automation (matched by localized name) — language-proof,
    // unlike the old Alt+F, D KeyTips which only worked on English Office.
    // A COM launch is kept as a fallback for the rare case with no resolved path.
    QString wordExe = findWordPath();
    QString wordExeEsc = wordExe;
    wordExeEsc.replace("'", "''"); // safe inside a single-quoted PS string

    CrashHandler::log(QString("Open Word Account clicked; resolved WINWORD path: '%1'")
                          .arg(wordExe.isEmpty() ? "(none, will try COM)" : wordExe));

    // Reset the diagnostic log and stamp it from C++ *before* launching PowerShell,
    // so a stale log can never mislead: if after a click the log shows only this
    // marker, PowerShell never ran (parse/launch failure); if it shows "ps start"
    // too, the script ran and the following lines say how far it got.
    const QString logPath = QDir::temp().filePath("qr_word_account.log");
    {
        QFile lf(logPath);
        if (lf.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            QTextStream ls(&lf);
            ls << "handler invoked " << QDateTime::currentDateTime().toString(Qt::ISODate)
               << "  wordExe='" << (wordExe.isEmpty() ? "(none)" : wordExe) << "'\n";
        }
    }

    QString ps = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$log = Join-Path $env:TEMP 'qr_word_account.log'
"ps start $(Get-Date -Format o)" | Add-Content -Path $log
$wordExe = '__WORDEXE__'
$fgOk = $false
try {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FG {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
}
"@
    $fgOk = $true
} catch { "addtype failed: $_" | Add-Content -Path $log }

$hwnd = [IntPtr]::Zero
try {
    if ($wordExe -and (Test-Path $wordExe)) {
        # Open with a blank document (a trivial RTF) so the ribbon is shown and
        # Alt+F -> D reaches Account. Launched with no file, Word lands on the
        # Start screen where that navigation does nothing.
        $blank = Join-Path $env:TEMP 'qr_blank.rtf'
        '{\rtf1\ansi }' | Set-Content -Path $blank -Encoding Ascii
        Start-Process -FilePath $wordExe -ArgumentList ('"' + $blank + '"')
        "started by path with blank doc" | Add-Content -Path $log
        for ($i = 0; $i -lt 100; $i++) {
            $p = Get-Process WINWORD -ErrorAction SilentlyContinue |
                 Where-Object { $_.MainWindowHandle -ne 0 } |
                 Sort-Object StartTime -Descending | Select-Object -First 1
            if ($p) { $hwnd = $p.MainWindowHandle; break }
            Start-Sleep -Milliseconds 200
        }
    } else {
        # Fallback: out-of-process COM (may fail under elevation).
        $w = New-Object -ComObject Word.Application
        $w.Visible = $true
        if ($w.Documents.Count -eq 0) { $w.Documents.Add() | Out-Null }
        $hwnd = [IntPtr]$w.ActiveWindow.Hwnd
        "started by COM" | Add-Content -Path $log
    }
} catch { "launch failed: $_" | Add-Content -Path $log }

if ($hwnd -eq [IntPtr]::Zero) {
    "no Word window found" | Add-Content -Path $log
    return
}
"hwnd $hwnd" | Add-Content -Path $log

# Bring Word to the foreground (best-effort, so the Account page is visible).
try {
    if ($fgOk) {
        for ($i = 0; $i -lt 30; $i++) {
            if ([FG]::IsIconic($hwnd)) { [FG]::ShowWindow($hwnd, 9) | Out-Null } # SW_RESTORE
            [FG]::SetForegroundWindow($hwnd) | Out-Null
            Start-Sleep -Milliseconds 100
            if ([FG]::GetForegroundWindow() -eq $hwnd) { break }
        }
    }
} catch { "foreground failed: $_" | Add-Content -Path $log }

# Navigate to the Account backstage page via UI Automation. This matches the
# File/Account items by their (localized) names and invokes them directly, so it
# works regardless of Office display language — unlike KeyTips, whose letters are
# localized (the old Alt+F, D only worked on English Office).
try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    Start-Sleep -Milliseconds 900  # let the document window settle

    $fileNames = @('File','文件','Fichier','Datei','Archivo','Arquivo','Ficheiro','ファイル','파일','Файл','Bestand','Fil','Tiedosto','Plik','Soubor','Fájl','Dosya','Fișier')
    $acctNames = @('Account','账户','帐户','帳戶','帳號','アカウント','계정','Compte','Konto','Cuenta','Conta','Учетная запись','Учётная запись','Cont','Účet','Račun','Hesap','Fiók','Tili','Office Account')

    function Find-ByNames($root, $names) {
        $conds = @()
        foreach ($nm in $names) {
            $conds += New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, $nm)
        }
        if ($conds.Count -eq 0) { return $null }
        if ($conds.Count -eq 1) { $cond = $conds[0] } else { $cond = New-Object System.Windows.Automation.OrCondition($conds) }
        return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
    }
    function Invoke-Element($el) {
        if (-not $el) { return $false }
        try { $el.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); return $true } catch {}
        try { $el.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select(); return $true } catch {}
        return $false
    }

    $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $acct = Find-ByNames $root $acctNames
    if (-not $acct) {
        $file = Find-ByNames $root $fileNames
        if (Invoke-Element $file) { "invoked File tab" | Add-Content -Path $log }
        else { "File tab not invoked" | Add-Content -Path $log }
        Start-Sleep -Milliseconds 1100
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        $acct = Find-ByNames $root $acctNames
    }
    if (Invoke-Element $acct) {
        "invoked Account" | Add-Content -Path $log
    } else {
        "Account not found; dumping button names" | Add-Content -Path $log
        $btnCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
        $btns = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $btnCond)
        $btns | ForEach-Object { $_.Current.Name } | Where-Object { $_ } | Select-Object -First 80 | ForEach-Object { "btn: $_" | Add-Content -Path $log }
    }
} catch { "uia failed: $_" | Add-Content -Path $log }
)PS";
    ps.replace("__WORDEXE__", wordExeEsc);

    QString scriptPath = QDir::temp().filePath("open_word_account.ps1");
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&script);
        // Windows PowerShell 5.1 reads BOM-less files as the system ANSI codepage,
        // which mangles the non-ASCII (localized) names in the script and breaks
        // parsing. A UTF-8 BOM makes it decode the file as UTF-8.
        out.setEncoding(QStringConverter::Utf8);
        out.setGenerateByteOrderMark(true);
        out << ps;
        script.close();
        const bool ok = QProcess::startDetached("powershell", QStringList()
            << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-WindowStyle" << "Hidden"
            << "-File" << scriptPath);
        CrashHandler::log(QString("Open Word Account: powershell launch %1")
                              .arg(ok ? "OK" : "FAILED"));
    } else {
        CrashHandler::log("Open Word Account: could not write helper script");
    }
    #endif
}

// Turns off BitLocker/device encryption on the system drive: an elevated script
// runs `manage-bde -off` and writes progress to a file that a timer in the popup
// dialog polls; the dialog also offers pause/resume via manage-bde.
void MainWindow::onDeviceEncryptionClicked() {
    #ifdef Q_OS_WIN
    if (QMessageBox::question(this, getTranslation("device_encryption"),
            getTranslation("device_encryption_confirm"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QString drive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (drive.isEmpty()) drive = "C:";

    QString progressPath = QDir::temp().filePath("qr_decrypt_progress.txt");
    QString scriptPath   = QDir::temp().filePath("qr_decrypt.ps1");
    QFile::remove(progressPath);

    // Elevated script: starts decryption, then polls manage-bde and writes the
    // current encryption percentage to a progress file the app reads back.
    QString ps = QString(R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$drive = '%1'
$progress = '%2'
Set-Content -Path $progress -Value 'START' -Encoding ASCII

manage-bde -off $drive | Out-Null

while ($true) {
    $status = manage-bde -status $drive
    $pct = 0
    $line = $status | Select-String 'Percentage Encrypted'
    if ($line -and ($line.ToString() -match '([\d\.]+)\s*%')) { $pct = [double]$matches[1] }
    $conv = $status | Select-String 'Conversion Status'
    Set-Content -Path $progress -Value ("PCT:" + $pct) -Encoding ASCII
    if (($conv -and ($conv.ToString() -match 'Fully Decrypted')) -or ($pct -le 0)) {
        Set-Content -Path $progress -Value 'DONE' -Encoding ASCII
        break
    }
    Start-Sleep -Seconds 2
}
)PS").arg(drive, progressPath);

    QFile script(scriptPath);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, getTranslation("device_encryption"),
                              getTranslation("device_encryption_failed"));
        return;
    }
    { QTextStream out(&script); out << ps; }
    script.close();

    // Launch elevated (decryption requires administrator rights).
    QString params = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + scriptPath + "\"";
    HINSTANCE res = ShellExecuteW(nullptr, L"runas", L"powershell.exe",
                                  (LPCWSTR)params.toStdWString().c_str(),
                                  nullptr, SW_HIDE);
    if ((INT_PTR)res <= 32) {
        appendLog(getTranslation("device_encryption_failed"));
        QMessageBox::warning(this, getTranslation("device_encryption"),
                             getTranslation("device_encryption_failed"));
        return;
    }
    appendLog(getTranslation("device_encryption_progress"));

    // Progress popup that polls the progress file.
    QDialog *dlg = new QDialog(this);
    dlg->setWindowTitle(getTranslation("device_encryption"));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setMinimumWidth(420);

    auto *lay = new QVBoxLayout(dlg);
    auto *label = new QLabel(getTranslation("device_encryption_progress"), dlg);
    label->setWordWrap(true);
    lay->addWidget(label);

    auto *bar = new QProgressBar(dlg);
    bar->setRange(0, 100);
    bar->setValue(0);
    lay->addWidget(bar);

    // BitLocker conversion cannot be rolled back mid-way, but it can be
    // paused/resumed via manage-bde (the app already runs elevated).
    auto *pauseBtn = new QPushButton(getTranslation("decrypt_pause"), dlg);
    pauseBtn->setCheckable(true);
    auto *closeBtn = new QPushButton(getTranslation("close"), dlg);
    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    btnRow->addWidget(pauseBtn);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    connect(pauseBtn, &QPushButton::clicked, dlg, [this, pauseBtn, label, drive](bool paused) {
        const int rc = QProcess::execute("manage-bde",
                                         {paused ? "-pause" : "-resume", drive});
        if (rc != 0) {
            appendLog(QString("manage-bde %1 %2 failed (exit %3)")
                          .arg(paused ? "-pause" : "-resume", drive)
                          .arg(rc));
            pauseBtn->blockSignals(true);
            pauseBtn->setChecked(!paused);
            pauseBtn->blockSignals(false);
            return;
        }
        pauseBtn->setText(getTranslation(paused ? "decrypt_resume" : "decrypt_pause"));
        if (paused)
            label->setText(getTranslation("decrypt_paused"));
    });

    auto *timer = new QTimer(dlg);
    timer->setInterval(1500);
    connect(timer, &QTimer::timeout, dlg, [this, dlg, bar, label, timer, pauseBtn, progressPath, scriptPath]() {
        QFile f(progressPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return; // not written yet
        QString content = QString::fromLocal8Bit(f.readAll()).trimmed();
        f.close();

        if (content == "START") {
            label->setText(getTranslation("device_encryption_progress"));
            return;
        }
        if (content.startsWith("PCT:")) {
            double pct = content.mid(4).toDouble();   // % still encrypted
            int decrypted = qBound(0, int(100.0 - pct), 100);
            bar->setValue(decrypted);
            if (pauseBtn->isChecked())
                label->setText(getTranslation("decrypt_paused")
                               + QString(" (%1%)").arg(decrypted));
            else
                label->setText(getTranslation("device_encryption_progress")
                               + QString(" (%1%)").arg(decrypted));
        } else if (content == "DONE") {
            bar->setValue(100);
            label->setText(getTranslation("device_encryption_done"));
            pauseBtn->setEnabled(false);
            timer->stop();
            QFile::remove(progressPath);
            QFile::remove(scriptPath);
        }
    });
    timer->start();

    dlg->show();
    #endif
}
