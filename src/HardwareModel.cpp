// =============================================================================
// HardwareModel.cpp - hardware scanning and Windows disk/system operations.
//
// Sections (in file order)
//   1. Small helpers and one-shot actions: OS name, internet check, admin check,
//      Fast Startup, diskpart clear / online, safe eject (PnP).
//   2. WinRE (Windows Recovery Environment) inspection and the
//      "merge unallocated space across the recovery partition" flow.
//   3. extendVolume() and createVolume() (diskpart / PowerShell based).
//   4. WMI plumbing (initWmi / queryWmi / cleanupWmi).
//   5. Scan orchestration: runScan(), runStorageScan() and the background
//      probes (battery report, driver check, encryption status).
//   6. System clone engine: raw sector copy with VSS snapshot, GPT fix-up,
//      recovery-partition relocation and shrink-to-fit safety net.
//   7. Detail readers: getWindowsInfo, getStorageDetails (incl. SMART over
//      ATA/NVMe/USB bridges), getMotherboardInfo, getBatteryDetails.
//
// Nearly everything is Windows-only and wrapped in #ifdef Q_OS_WIN. All blocking
// functions are meant to run on a worker thread, never on the GUI thread.
// =============================================================================
#include "HardwareModel.h"
#include <QSysInfo>
#include <QStandardPaths>
#include <QDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QProcess>
#include <QTcpSocket>
#include <QSettings>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QTextStream>
#include <QSet>
#include <QStorageInfo>
#include <QElapsedTimer>
#include <QUuid>
#include <algorithm>
#include <chrono>
#include <limits>
#include <vector>
#include <thread>
#include "CrashHandler.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <dxgi.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#endif

// Constructor: WMI/COM state starts uninitialised; initWmi() is called per scan.
HardwareModel::HardwareModel(QObject *parent)
    : QObject(parent), wmiInitialized(false), comInitialized(false), m_pLoc(nullptr), m_pSvc(nullptr)
{
}

// Destructor: releases WMI/COM resources if a scan left them open.
HardwareModel::~HardwareModel() {
    cleanupWmi();
}

// Friendly OS name, e.g. "Windows 11 Pro".
QString HardwareModel::detectOs() {
    return QSysInfo::prettyProductName();
}

// Quick connectivity probe: TCP connect to 8.8.8.8:53 with a 3 s timeout.
bool HardwareModel::checkInternetConnection() {
    QTcpSocket socket;
    socket.connectToHost("8.8.8.8", 53);
    if (socket.waitForConnected(3000)) {
        return true;
    }
    return false;
}

// Directory of the running executable.
QString HardwareModel::getBaseDir() {
    return QCoreApplication::applicationDirPath();
}

// Finds the first file whose full path contains `pattern` (case-insensitive
// substring, not a glob). Searches baseDir recursively, then the parent folder
// (non-recursive; for the launcher layout). Returns "" if nothing matches.
QString HardwareModel::findFile(const QString &baseDir, const QString &pattern) {
    // Search in the base directory (application dir)
    QDir dir(baseDir);
    QDirIterator it(baseDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString file = it.next();
        if (file.contains(pattern, Qt::CaseInsensitive)) {
            return file;
        }
    }

    // Also search in the parent directory (useful for portable/launcher setup)
    if (dir.cdUp()) {
        QString parentDir = dir.absolutePath();
        QDirIterator itParent(parentDir, QDir::Files, QDirIterator::NoIteratorFlags); // Non-recursive in parent to avoid deep scans
        while (itParent.hasNext()) {
            QString file = itParent.next();
            if (file.contains(pattern, Qt::CaseInsensitive)) {
                return file;
            }
        }
    }

    return QString();
}

// True when the process token is elevated (same check as in main.cpp).
bool HardwareModel::isUserAdmin() {
    #ifdef Q_OS_WIN
    BOOL fRet = FALSE;
    HANDLE hToken = NULL;
    if( OpenProcessToken( GetCurrentProcess( ),TOKEN_QUERY,&hToken ) ) {
        TOKEN_ELEVATION Elevation;
        DWORD cbSize = sizeof( TOKEN_ELEVATION );
        if( GetTokenInformation( hToken, TokenElevation, &Elevation, sizeof( Elevation ), &cbSize ) ) {
            fRet = Elevation.TokenIsElevated;
        }
    }
    if( hToken ) {
        CloseHandle( hToken );
    }
    return fRet;
    #else
    return false;
    #endif
}

// Sets HiberbootEnabled=0 so shutdown fully releases disks (needed before disk
// operations). Returns "Success" or an explanation.
QString HardwareModel::disableFastStartup() {
    #ifdef Q_OS_WIN
    QSettings settings("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", QSettings::NativeFormat);
    if (settings.status() == QSettings::NoError) {
        settings.setValue("HiberbootEnabled", 0);
        if (settings.status() == QSettings::NoError) return "Success";
    }
    return "Failed to write registry key. Ensure Admin privileges.";
    #else
    return "Not supported on this OS.";
    #endif
}

// `diskpart clean` on the given disk number (all partitions and data are lost).
// Returns {success, diagnostic output}. Requires administrator rights.
QPair<bool, QString> HardwareModel::clearDisk(const QString &diskNum) {
    #ifdef Q_OS_WIN
    if (!isUserAdmin()) {
        return {false, "Administrator privileges are required."};
    }

    QString scriptFile = QDir::temp().filePath("diskpart_clean.txt");
    QFile file(scriptFile);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << "select disk " << diskNum << "\n";
        out << "clean\n";
        file.close();
    } else {
        return {false, "Failed to create temporary script file."};
    }

    QProcess process;
    process.start("diskpart", QStringList() << "/s" << scriptFile);
    process.waitForFinished();

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    QString error = QString::fromLocal8Bit(process.readAllStandardError());

    QFile::remove(scriptFile);

    if (process.exitCode() == 0) {
        return {true, output};
    } else {
        return {false, output + "\n" + error};
    }
    #else
    return {false, "Not supported on this OS."};
    #endif
}

#ifdef Q_OS_WIN
// Forward declaration; defined further down (used elsewhere for the storage
// scan). Reused here as the authoritative, language-independent success check
// for setDiskOnline — diskpart's own text is localized (French/Chinese diskpart
// never says "onlined"), so matching English words there always failed to
// recognize success on a non-English Windows install even though the disk did
// go online.
static bool readDiskIsOffline(const QString &diskNumber);
#endif

// Runs diskpart to online the disk and clear its read-only attribute; success is
// verified by re-reading the disk state, not by parsing diskpart's text.
QPair<bool, QString> HardwareModel::setDiskOnline(const QString &diskNum) {
    #ifdef Q_OS_WIN
    if (!isUserAdmin()) {
        return {false, "Administrator privileges are required."};
    }

    QString scriptFile = QDir::temp().filePath("uqr_online.txt");
    QFile file(scriptFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {false, "Failed to create temporary script file."};
    }
    {
        QTextStream out(&file);
        out << "select disk " << diskNum << "\n"
            << "attributes disk clear readonly noerr\n"
            << "online disk noerr\n"
            << "exit\n";
    }
    file.close();

    QProcess process;
    process.start("diskpart", QStringList() << "/s" << scriptFile);
    process.waitForFinished(-1);
    // diskpart is a console program: decode with the local (OEM/ANSI) codepage,
    // not UTF-8, or its output (English or localized) comes out as mojibake.
    QString output = QString::fromLocal8Bit(
        process.readAllStandardOutput() + "\n" + process.readAllStandardError()).trimmed();
    QFile::remove(scriptFile);

    // diskpart's exit code is unreliable, and its message text is localized (a
    // Chinese or French diskpart never prints the English "onlined"), so ask
    // Windows directly whether the disk is actually online now.
    if (!readDiskIsOffline(diskNum))
        return {true, output};
    return {false, output};
    #else
    Q_UNUSED(diskNum);
    return {false, "Not supported on this OS."};
    #endif
}

#ifdef Q_OS_WIN
// GUID_DEVINTERFACE_DISK — spelled out locally so we don't have to pull in
// <initguid.h> (which would force every DEFINE_GUID visible here to be
// allocated in this translation unit).
static const GUID kDiskInterfaceGuid =
    { 0x53f56307, 0xb6bf, 0x11d0, { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b } };

// True when PnP marks the devnode removable (i.e. it can be safely ejected).
static bool devNodeIsRemovable(DEVINST devInst) {
    ULONG caps = 0;
    ULONG len = sizeof(caps);
    if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_CAPABILITIES,
                                           nullptr, &caps, &len, 0) != CR_SUCCESS)
        return false;
    return (caps & CM_DEVCAP_REMOVABLE) != 0;
}

// Locates the device instance of the physical drive carrying the given disk
// number by matching IOCTL_STORAGE_GET_DEVICE_NUMBER over the disk interfaces.
static DEVINST diskDevInstByNumber(int diskNumber) {
    HDEVINFO info = SetupDiGetClassDevsW(&kDiskInterfaceGuid, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE)
        return 0;

    DEVINST found = 0;
    SP_DEVICE_INTERFACE_DATA ifd;
    ifd.cbSize = sizeof(ifd);
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(info, nullptr, &kDiskInterfaceGuid, idx, &ifd);
         ++idx) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifd, nullptr, 0, &needed, nullptr);
        if (needed == 0)
            continue;
        std::vector<BYTE> buf(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo;
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifd, detail, needed, nullptr, &devInfo))
            continue;

        HANDLE h = CreateFileW(detail->DevicePath, 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        STORAGE_DEVICE_NUMBER sdn = {};
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0,
                                  &sdn, sizeof(sdn), &ret, nullptr);
        CloseHandle(h);
        if (ok && static_cast<int>(sdn.DeviceNumber) == diskNumber) {
            found = devInfo.DevInst;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

// Best-effort flush: lock and dismount every logical volume that lives on the
// disk, so the eject completes cleanly and is less likely to be vetoed.
static void dismountVolumesOnDisk(int diskNumber) {
    WCHAR volName[MAX_PATH];
    HANDLE find = FindFirstVolumeW(volName, MAX_PATH);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        const size_t n = wcslen(volName);
        // A volume-GUID path opened with a trailing backslash fails; strip it.
        if (n > 0 && volName[n - 1] == L'\\')
            volName[n - 1] = 0;
        HANDLE hVol = CreateFileW(volName, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (n > 0)
            volName[n - 1] = L'\\';   // restore for FindNextVolume
        if (hVol == INVALID_HANDLE_VALUE)
            continue;

        BYTE extBuf[sizeof(VOLUME_DISK_EXTENTS) + 32 * sizeof(DISK_EXTENT)] = {};
        auto *ext = reinterpret_cast<VOLUME_DISK_EXTENTS *>(extBuf);
        DWORD ret = 0;
        bool onThisDisk = false;
        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                            ext, sizeof(extBuf), &ret, nullptr)) {
            for (DWORD i = 0; i < ext->NumberOfDiskExtents; ++i) {
                if (static_cast<int>(ext->Extents[i].DiskNumber) == diskNumber) {
                    onThisDisk = true;
                    break;
                }
            }
        }
        if (onThisDisk) {
            DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr);
            DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr);
        }
        CloseHandle(hVol);
    } while (FindNextVolumeW(find, volName, MAX_PATH));
    FindVolumeClose(find);
}
#endif

// Safe removal: dismount volumes, then ask PnP to eject the topmost removable
// ancestor of the drive's devnode. On failure the string explains why (in use, ...).
QPair<bool, QString> HardwareModel::ejectDisk(const QString &diskNum) {
    #ifdef Q_OS_WIN
    bool okNum = false;
    const int diskNumber = diskNum.toInt(&okNum);
    if (!okNum || diskNumber < 0)
        return {false, "Invalid disk number."};

    DEVINST devInst = diskDevInstByNumber(diskNumber);
    if (devInst == 0)
        return {false, "Could not locate the device for this disk."};

    // Walk up to the topmost removable ancestor. The USB mass-storage / disk
    // node is removable while its parent hub is not, so we stop at the hub and
    // eject the drive itself rather than the whole port.
    DEVINST target = devInst;
    DEVINST parent = 0;
    while (CM_Get_Parent(&parent, target, 0) == CR_SUCCESS) {
        if (!devNodeIsRemovable(parent))
            break;
        target = parent;
    }

    // Flush and unmount the volumes first (best effort).
    dismountVolumesOnDisk(diskNumber);

    // Ask PnP to eject. Retry a few times: a transient handle (indexing, AV,
    // Explorer preview) can veto the first attempt and clear moments later.
    PNP_VETO_TYPE vetoType = PNP_VetoTypeUnknown;
    WCHAR vetoName[MAX_PATH];
    CONFIGRET cr = CR_FAILURE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        vetoName[0] = 0;
        vetoType = PNP_VetoTypeUnknown;
        cr = CM_Request_Device_EjectW(target, &vetoType, vetoName, MAX_PATH, 0);
        if (cr == CR_SUCCESS && vetoType == PNP_VetoTypeUnknown)
            return {true, QString()};
        Sleep(700);
    }

    if (cr != CR_SUCCESS)
        return {false, QStringLiteral("PnP eject request failed (code %1).").arg(cr)};
    const QString who = QString::fromWCharArray(vetoName).trimmed();
    return {false, who.isEmpty()
                       ? QStringLiteral("A program is still using the disk.")
                       : who};
    #else
    Q_UNUSED(diskNum);
    return {false, "Not supported on this OS."};
    #endif
}

#ifdef Q_OS_WIN
// Reads WinRE state from `reagentc /info`. Returns true when WinRE is enabled,
// setting *diskNum to the physical disk hosting it. Detection keys off the
// "harddiskN" token in the location path, which — unlike the "Enabled/Disabled"
// word — is not localized, so this works on any Windows display language.
static bool readWinReInfo(QString *diskNum)
{
    QProcess p;
    p.start("reagentc", QStringList() << "/info");
    if (!p.waitForFinished(8000)) {
        p.kill();
        return false;
    }
    const QString out = QString::fromLocal8Bit(
        p.readAllStandardOutput() + p.readAllStandardError());
    QRegularExpression re("harddisk(\\d+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = re.match(out);
    if (m.hasMatch()) {
        if (diskNum)
            *diskNum = m.captured(1);
        return true;
    }
    return false;
}

static void fillWinReDiagnostics(StorageDisk &disk, bool enabled,
                                 const QString &location)
{
    disk.winReStatus = enabled ? "Enabled" : "Disabled";
    disk.winReLocation = location;

    // A GPT Recovery partition uses this fixed Microsoft type GUID.  On MBR,
    // WMI does not expose the type reliably, so the size/name heuristic remains
    // useful while recoveryTypeCorrect stays false/unknown.
    const QString script = QString(
        "$ErrorActionPreference='SilentlyContinue';"
        "$r=Get-Partition -DiskNumber %1 | Where-Object {"
        " $_.Type -eq 'Recovery' -or ([string]$_.GptType).Trim('{}').ToLower() -eq 'de94bba4-06d1-4d40-a16a-bfd50179d6ac'"
        "} | Select-Object -First 1;"
        "if($r){Write-Output ('UQR|'+$r.PartitionNumber+'|'+$r.Size+'|'+([string]$r.GptType))}"
    ).arg(disk.diskNumber);
    QProcess p;
    p.start("powershell", {"-NoProfile", "-Command", script});
    if (p.waitForFinished(8000)) {
        const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
        QRegularExpression rx("UQR\\|(\\d+)\\|(\\d+)\\|([^\\r\\n]*)");
        const auto m = rx.match(out);
        if (m.hasMatch()) {
            disk.hasRecoveryPartition = true;
            disk.winRePartitionNumber = m.captured(1);
            disk.winRePartitionSize = m.captured(2).toLongLong();
            disk.recoveryTypeCorrect = m.captured(3).contains(
                "de94bba4-06d1-4d40-a16a-bfd50179d6ac", Qt::CaseInsensitive);
        }
    } else {
        p.kill();
    }

    if (!location.isEmpty()) {
        const QString imagePath = QDir::toNativeSeparators(location + "/Winre.wim");
        disk.winReImageKnown = true;
        disk.winReImagePresent = QFileInfo::exists(imagePath);
    } else {
        const QString fallback = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
                                     .filePath("System32/Recovery/Winre.wim");
        disk.winReImageKnown = QFileInfo::exists(fallback);
        disk.winReImagePresent = disk.winReImageKnown;
    }
}
#endif

#ifdef Q_OS_WIN
// One partition on the system disk, as read live from Get-Partition. Offsets and
// sizes are the physical byte positions we key the layout analysis off of.
struct LivePartition {
    int number = 0;
    qint64 offset = 0;
    qint64 size = 0;
    bool isRecovery = false;
    QChar driveLetter; // null QChar when the partition has no letter
};

// Run a diskpart script (written to a temp file) and return its combined output.
// diskpart does not reliably set a non-zero exit code, so callers verify success
// from the text / from a follow-up state read rather than the process result.
static QString runDiskpartScript(const QString &script)
{
    const QString scriptFile = QDir::temp().filePath("uqr_mergerec.txt");
    QFile file(scriptFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("Failed to create temporary diskpart script file.");
    {
        QTextStream out(&file);
        out << script;
    }
    file.close();

    QProcess process;
    process.start("diskpart", QStringList() << "/s" << scriptFile);
    process.waitForFinished(-1);
    const QString output = QString::fromLocal8Bit(
        process.readAllStandardOutput() + "\n" + process.readAllStandardError());
    QFile::remove(scriptFile);
    return output.trimmed();
}

// Reads a disk's live partition layout straight from Windows (never a cached
// scan), for the merge-across-recovery flows (system and non-system). Shared
// so both read the exact same "is this a recovery partition" definition.
static bool probeLiveDiskLayout(int diskNumber, qint64 &diskSize, QString &partitionStyle,
                                QList<LivePartition> &parts)
{
    const QString probe = QString(
        "$ErrorActionPreference='SilentlyContinue';"
        "$d=%1;"
        "$disk=Get-Disk -Number $d;"
        "Write-Output ('DISK|'+$disk.Size+'|'+$disk.PartitionStyle);"
        "Get-Partition -DiskNumber $d | Sort-Object Offset | ForEach-Object {"
        " $rec = ($_.Type -eq 'Recovery') -or"
        " (([string]$_.GptType).Trim('{}').ToLower() -eq 'de94bba4-06d1-4d40-a16a-bfd50179d6ac');"
        " $dl = if($_.DriveLetter){[string]$_.DriveLetter}else{''};"
        " Write-Output ('PART|'+$_.PartitionNumber+'|'+$_.Offset+'|'+$_.Size+'|'+$rec+'|'+$dl)"
        "}").arg(diskNumber);
    QProcess probeProc;
    probeProc.start("powershell", {"-NoProfile", "-Command", probe});
    if (!probeProc.waitForFinished(15000)) {
        probeProc.kill();
        return false;
    }
    const QStringList lines = QString::fromLocal8Bit(probeProc.readAllStandardOutput())
                                  .split('\n', Qt::SkipEmptyParts);
    diskSize = 0;
    partitionStyle.clear();
    parts.clear();
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith("DISK|")) {
            const QStringList f = line.split('|');
            if (f.size() >= 3) { diskSize = f.at(1).toLongLong(); partitionStyle = f.at(2).trimmed(); }
        } else if (line.startsWith("PART|")) {
            const QStringList f = line.split('|');
            if (f.size() >= 5) {
                LivePartition lp;
                lp.number     = f.at(1).toInt();
                lp.offset     = f.at(2).toLongLong();
                lp.size       = f.at(3).toLongLong();
                lp.isRecovery = f.at(4).trimmed().compare("True", Qt::CaseInsensitive) == 0;
                if (f.size() >= 6 && !f.at(5).trimmed().isEmpty())
                    lp.driveLetter = f.at(5).trimmed().at(0).toUpper();
                parts.append(lp);
            }
        }
    }
    return diskSize > 0 && !parts.isEmpty();
}

#endif

QPair<bool, QString> HardwareModel::mergeUnallocatedAcrossRecovery(
    const WinReProgress &progress)
{
    #ifdef Q_OS_WIN
    auto report = [&progress](int percent, const QString &stage) {
        if (progress)
            progress(percent, stage);
    };
    report(2, "checking");
    if (!isUserAdmin())
        return {false, "Administrator privileges are required."};

    auto runReagentc = [](const QStringList &args, int timeoutMs = 30000) {
        QProcess process;
        process.start("reagentc", args);
        if (!process.waitForFinished(timeoutMs)) {
            process.kill();
            process.waitForFinished();
            return QString("reagentc timed out: %1").arg(args.join(' '));
        }
        return QString::fromLocal8Bit(
            process.readAllStandardOutput() + process.readAllStandardError()).trimmed();
    };

    // ── resolve the system disk number ──────────────────────────────────────
    // WinRE's harddiskN is the most reliable when WinRE is enabled; otherwise
    // fall back to the disk that hosts the system drive letter.
    QString sysDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
    const QChar sysLetter = (sysDrive.size() >= 1) ? sysDrive.at(0).toUpper() : QChar('C');
    QString diskNumStr;
    readWinReInfo(&diskNumStr);
    if (diskNumStr.isEmpty()) {
        QProcess p;
        p.start("powershell", {"-NoProfile", "-Command",
            QString("$ErrorActionPreference='SilentlyContinue';"
                    "(Get-Partition -DriveLetter %1).DiskNumber").arg(sysLetter)});
        if (p.waitForFinished(8000))
            diskNumStr = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    }
    bool okDisk = false;
    const int diskNumber = diskNumStr.toInt(&okDisk);
    if (!okDisk)
        return {false, "Could not identify the system disk."};

    // ── read the live partition layout (never trust a cached scan here) ──────
    qint64 diskSize = 0;
    QString partitionStyle;
    QList<LivePartition> parts;
    if (!probeLiveDiskLayout(diskNumber, diskSize, partitionStyle, parts))
        return {false, "Could not read the disk layout."};

    const bool isGpt = partitionStyle.compare("GPT", Qt::CaseInsensitive) == 0;

    // Locate the system volume (C:) and everything physically after it.
    const LivePartition *sysPart = nullptr;
    for (const auto &p : parts)
        if (p.driveLetter == sysLetter) { sysPart = &p; break; }
    if (!sysPart)
        return {false, "Could not find the system volume on the system disk."};
    const qint64 sysEnd = sysPart->offset + sysPart->size;

    // ── safety: every partition after C: must be a recovery partition ────────
    QList<int> recoveryNums;      // partition numbers to delete
    qint64 recoveryTotal = 0;
    qint64 lastEnd = sysEnd;
    for (const auto &p : parts) {
        if (p.offset < sysEnd)
            continue;                       // before / at C: — ignore
        if (!p.isRecovery)
            return {false, "__DATA_PARTITION__"}; // a real partition blocks — refuse
        recoveryNums.append(p.number);
        recoveryTotal += p.size;
        lastEnd = qMax(lastEnd, p.offset + p.size);
    }
    if (recoveryNums.isEmpty())
        return {false, "No recovery partition sits between the system volume and the free space."};

    // Free space that becomes contiguous after C: once the recovery partitions
    // are gone. Keep a 2 MiB tail margin for the GPT backup / 1 MiB alignment.
    const qint64 mib = 1024LL * 1024;
    const qint64 tailMargin = 2 * mib;
    const qint64 freeAfterDelete = diskSize - sysEnd - tailMargin;
    // Reserve for the rebuilt recovery partition: keep the original size but no
    // less than 750 MiB, rounded up to a MiB and padded by 1 MiB for alignment.
    qint64 reserveBytes = qMax<qint64>(recoveryTotal, 750LL * mib);
    reserveBytes = ((reserveBytes + mib - 1) / mib) * mib + mib;
    const qint64 extendBytes = freeAfterDelete - reserveBytes;
    const qint64 extendMB = extendBytes / mib;
    if (extendMB < 100)
        return {false, "There is not enough tail unallocated space to meaningfully extend the system volume."};

    CrashHandler::log(QString("Merge-across-recovery: disk %1 (%2), C: end=%3, recovery x%4 total=%5, extend=%6 MiB, reserve=%7 MiB")
                          .arg(diskNumber).arg(partitionStyle)
                          .arg(sysEnd).arg(recoveryNums.size()).arg(recoveryTotal)
                          .arg(extendMB).arg(reserveBytes / mib));

    // ── step 1: move WinRE back onto C: (reversible, non-destructive) ────────
    report(15, "disabling_winre");
    runReagentc(QStringList() << "/disable");
    const QString recoveryDir = QDir::toNativeSeparators(
        QDir::cleanPath(qEnvironmentVariable("SystemRoot", "C:/Windows")
                        + "/System32/Recovery"));
    const QString winreOnC = QDir(recoveryDir).filePath("Winre.wim");
    if (!QFileInfo::exists(winreOnC)) {
        // reagentc /disable did not leave an image (WinRE was already off, or a
        // stale registration). Fall back to the bundled portable image so we can
        // rebuild + re-enable WinRE after the resize; abort if none is available
        // — we must never delete the recovery partition without a way back.
        const QString bundled = QDir(QCoreApplication::applicationDirPath())
                                    .filePath("WinRE/Winre.wim");
        const QFileInfo bundledInfo(bundled);
        if (!bundledInfo.isFile() || bundledInfo.size() < 100LL * 1024 * 1024)
            return {false, "Windows RE image is not available on C:, and no valid bundled "
                           "WinRE/Winre.wim was found. Aborted before touching the disk."};
        if (!QDir().mkpath(recoveryDir))
            return {false, "Could not create the Windows Recovery directory."};
        if (!QFile::copy(bundled, winreOnC))
            return {false, "Could not stage the bundled Winre.wim onto C:. Aborted before touching the disk."};
    }

    // ── step 2: delete recovery partition(s), extend C:, rebuild recovery ────
    report(35, "critical"); // from here the layout changes; UI locks out cancel
    report(40, "deleting_recovery");
    QString script;
    QTextStream s(&script);
    s << "select disk " << diskNumber << "\n";
    // Delete highest partition number first so earlier selections don't shift.
    std::sort(recoveryNums.begin(), recoveryNums.end(),
              [](int a, int b) { return a > b; });
    for (int n : recoveryNums) {
        s << "select partition " << n << "\n";
        s << "delete partition override\n";
    }
    s << "select volume " << sysLetter << "\n";
    s << "extend size=" << extendMB << "\n";
    s << "create partition primary\n";
    if (isGpt) {
        s << "set id=\"de94bba4-06d1-4d40-a16a-bfd50179d6ac\"\n";
        s << "gpt attributes=0x8000000000000001\n";
    } else {
        s << "set id=27\n";
    }
    s << "format quick fs=ntfs label=\"Recovery\"\n";
    s << "exit\n";

    report(55, "extending");
    const QString dpOut = runDiskpartScript(script);
    report(80, "rebuilding_recovery");

    // Verify the system volume actually grew: re-read C:'s size. This is the
    // real success signal, since diskpart's text/exit code is unreliable.
    auto readSysSize = [sysLetter]() -> qint64 {
        QProcess p;
        p.start("powershell", {"-NoProfile", "-Command",
            QString("$ErrorActionPreference='SilentlyContinue';"
                    "(Get-Partition -DriveLetter %1).Size").arg(sysLetter)});
        if (!p.waitForFinished(8000)) { p.kill(); return -1; }
        return QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed().toLongLong();
    };
    const qint64 newSysSize = readSysSize();
    const bool extended = newSysSize > sysPart->size + (extendMB - 8) * mib;

    // ── step 3: re-enable WinRE (into the new recovery partition) ────────────
    report(90, "enabling");
    runReagentc(QStringList() << "/setreimage" << "/path" << recoveryDir);
    runReagentc(QStringList() << "/enable");
    report(97, "verifying");
    QString reDisk;
    const bool winReOk = readWinReInfo(&reDisk);

    if (extended) {
        report(100, "complete");
        QString msg = QString("System volume extended by ~%1 GiB.").arg((extendMB) / 1024.0, 0, 'f', 1);
        if (!winReOk)
            msg += " (Windows RE could not be re-enabled automatically; use Restore Windows RE.)";
        return {true, msg};
    }
    // Extend failed. C: is untouched and still bootable; WinRE was re-enabled
    // (on C: if the new partition wasn't usable), so recovery still works.
    return {false, QString("The system volume could not be extended.\n%1").arg(dpOut)};
    #else
    Q_UNUSED(progress);
    return {false, "Not supported on this OS."};
    #endif
}

// Extends a volume into the unallocated space directly behind it
// (diskpart `extend`). Used for the plain C: extend case.
QPair<bool, QString> HardwareModel::extendVolume(const QString &driveLetter) {
    #ifdef Q_OS_WIN
    if (!isUserAdmin()) {
        return {false, "Administrator privileges are required."};
    }

    QString letter = driveLetter;
    letter.remove(':');
    letter = letter.trimmed();
    if (letter.isEmpty()) {
        return {false, "Invalid drive letter."};
    }

    // "extend" (no size) grows the volume into all contiguous unallocated space
    // that immediately follows it on the same disk.
    QString scriptFile = QDir::temp().filePath("uqr_extend.txt");
    QFile file(scriptFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {false, "Failed to create temporary script file."};
    }
    {
        QTextStream out(&file);
        out << "select volume " << letter << "\n";
        out << "extend\n";
        out << "exit\n";
    }
    file.close();

    QProcess process;
    process.start("diskpart", QStringList() << "/s" << scriptFile);
    process.waitForFinished(-1);

    QString output = QString::fromLocal8Bit(process.readAllStandardOutput() + "\n"
                                            + process.readAllStandardError());
    QFile::remove(scriptFile);

    // diskpart doesn't always set a non-zero exit code on failure, so also check text.
    if (output.contains("successfully extended", Qt::CaseInsensitive))
        return {true, output.trimmed()};
    return {false, output.trimmed()};
    #else
    return {false, "Not supported on this OS."};
    #endif
}

#ifdef Q_OS_WIN
// Auto-dismiss the transient Windows shell popups that can race disk init:
// "You need to format the disk in drive X:…" (dialog titled "Microsoft Windows")
// and "Location is not available" (localized). We only touch dialogs (window
// class #32770) owned by explorer.exe and post WM_CLOSE — that is the Cancel
// action, so it never triggers a format.
static BOOL CALLBACK closeShellVolumeDlgProc(HWND hwnd, LPARAM)
{
    if (!IsWindowVisible(hwnd))
        return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);
    if (wcscmp(cls, L"#32770") != 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return TRUE;
    bool isExplorer = false;
    if (HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH] = {};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hp, 0, path, &sz))
            isExplorer = QString::fromWCharArray(path)
                             .endsWith("\\explorer.exe", Qt::CaseInsensitive);
        CloseHandle(hp);
    }
    if (!isExplorer)
        return TRUE;

    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 255);
    const QString t = QString::fromWCharArray(title);
    // Simplified-Chinese "Location is not available" built from code points so
    // this source file stays pure ASCII (no UTF-8 BOM here, so a raw literal
    // would be mis-decoded by MSVC).
    static const QString locZh = QString(QChar(0x4F4D)) + QChar(0x7F6E)
                               + QChar(0x4E0D) + QChar(0x53EF) + QChar(0x7528);
    static const QStringList knownTitles = {
        QStringLiteral("Microsoft Windows"),         // "format this disk?" prompt
        QStringLiteral("Location is not available"), // en
        locZh,                                       // zh-CN
    };
    for (const QString &known : knownTitles) {
        if (t.compare(known, Qt::CaseInsensitive) == 0) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0); // = Cancel; never formats
            break;
        }
    }
    return TRUE;
}

// Sweep for the popups above for durationMs. Runs on its own thread so it can
// close a dialog that appears while diskpart blocks the caller.
static void dismissShellVolumeDialogs(int durationMs)
{
    const int step = 200;
    for (int elapsed = 0; elapsed < durationMs; elapsed += step) {
        EnumWindows(closeShellVolumeDlgProc, 0);
        Sleep(step);
    }
}
#endif // Q_OS_WIN

static int runPowerShellScript(const QString &script, QString *out, QString *err) {
    const QByteArray encoded = QByteArray(
        reinterpret_cast<const char *>(script.utf16()), int(script.size()) * 2).toBase64();
    QProcess process;
    process.start("powershell.exe", QStringList()
        << "-NoProfile" << "-NonInteractive" << "-ExecutionPolicy" << "Bypass"
        << "-EncodedCommand" << QString::fromLatin1(encoded));
    process.waitForFinished(-1);
    if (out) *out = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (err) *err = QString::fromLocal8Bit(process.readAllStandardError());
    return process.exitCode();
}

// Creates a partition in the disk's free space and formats/mounts it according to
// the New Volume Wizard options. Initialises the disk first when asked. Returns
// {success, mount location or diagnostic output}.
QPair<bool, QString> HardwareModel::createVolume(const NewVolumeOptions &opt) {
    #ifdef Q_OS_WIN
    if (!isUserAdmin()) {
        return {false, "Administrator privileges are required."};
    }

    // Uses the Storage Management cmdlets (the same API Disk Management uses)
    // rather than diskpart: on some USB bridges diskpart's 'format' fails with
    // "The parameter is incorrect" where Disk Management succeeds. The script is
    // passed as -EncodedCommand (UTF-16), so non-ASCII labels/paths survive intact.
    const QString fs = opt.fileSystem.trimmed().toLower();
    QString psFs;
    if (fs == "ntfs") psFs = "NTFS";
    else if (fs == "exfat") psFs = "exFAT";
    else if (fs == "fat32") psFs = "FAT32";
    else return {false, "Unsupported file system."};
    if (fs == "fat32" && opt.sizeMB > 32 * 1024)
        return {false, "FAT32 volumes larger than 32 GB are not supported."};

    bool okNum = false;
    const int diskNum = opt.diskNum.toInt(&okNum);
    if (!okNum || diskNum < 0)
        return {false, "Invalid disk number."};

    QString letter = opt.driveLetter.trimmed().toUpper();
    if (opt.mount == NewVolumeOptions::Letter) {
        if (letter.size() != 1 || letter[0] < QChar('A') || letter[0] > QChar('Z'))
            return {false, "Invalid drive letter."};
        if (GetLogicalDrives() & (1u << (letter[0].toLatin1() - 'A')))
            return {false, QStringLiteral("Drive letter %1: is already in use.").arg(letter)};
    }
    QString folder;
    if (opt.mount == NewVolumeOptions::Folder) {
        folder = QDir::toNativeSeparators(opt.mountFolder);
        if (!QFileInfo(folder).isDir() ||
            !QDir(folder).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            return {false, "The mount folder must be an existing, empty folder."};
        if (!folder.endsWith('\\')) folder += '\\';
    }

    auto q = [](const QString &s) { // PowerShell single-quoted literal
        QString t = s;
        t.replace('\'', QStringLiteral("''"));
        return QStringLiteral("'") + t + QStringLiteral("'");
    };
    QString label = opt.label;
    label.remove(QRegularExpression(QStringLiteral("[\\r\\n\\t]")));
    label = label.trimmed().left(32);

    QStringList ps;
    ps << "$ErrorActionPreference = 'Stop'"
       << "try {"
       << QStringLiteral("  $n = %1").arg(diskNum)
       << "  $d = Get-Disk -Number $n"
       << "  if ($d.IsOffline) { Set-Disk -Number $n -IsOffline $false }"
       << "  if ($d.IsReadOnly) { Set-Disk -Number $n -IsReadOnly $false }";
    if (opt.initializeDisk) {
        // Only ever set for a disk with no partitions, so wiping it is safe.
        ps << "  if ($d.PartitionStyle -ne 'RAW') { Clear-Disk -Number $n -RemoveData -RemoveOEM -Confirm:$false }"
           << QStringLiteral("  Initialize-Disk -Number $n -PartitionStyle %1")
                  .arg(opt.partitionStyle.toLower() == "mbr" ? "MBR" : "GPT");
    }
    // No -AssignDriveLetter: the raw partition gets no letter, so Explorer never
    // pops up "You need to format the disk" before we format it.
    if (opt.sizeMB > 0)
        ps << QStringLiteral("  $p = New-Partition -DiskNumber $n -Size %1").arg(opt.sizeMB * 1024 * 1024);
    else
        ps << "  $p = New-Partition -DiskNumber $n -UseMaximumSize";
    QString fmt = QStringLiteral("  $p | Format-Volume -FileSystem %1 -Confirm:$false").arg(psFs);
    if (opt.unitBytes > 0)
        fmt += QStringLiteral(" -AllocationUnitSize %1").arg(opt.unitBytes);
    if (!label.isEmpty())
        fmt += QStringLiteral(" -NewFileSystemLabel %1").arg(q(label));
    if (!opt.quickFormat)
        fmt += QStringLiteral(" -Full");
    ps << fmt + " | Out-Null";

    const QString sel = "-DiskNumber $n -PartitionNumber $p.PartitionNumber";
    if (opt.mount == NewVolumeOptions::Letter)
        ps << QStringLiteral("  Set-Partition %1 -NewDriveLetter %2").arg(sel, letter);
    else if (opt.mount == NewVolumeOptions::AutoLetter)
        ps << QStringLiteral("  Add-PartitionAccessPath %1 -AssignDriveLetter").arg(sel);
    else if (opt.mount == NewVolumeOptions::Folder)
        ps << QStringLiteral("  Add-PartitionAccessPath %1 -AccessPath %2").arg(sel, q(folder));
    ps << QStringLiteral("  $pp = Get-Partition %1").arg(sel)
       << "  Write-Output ('RESULT|' + $pp.DriveLetter)"
       << "} catch {"
       << "  Write-Output ('ERROR|' + $_.Exception.Message)"
       << "  exit 1"
       << "}";

    std::thread([] { dismissShellVolumeDialogs(12000); }).detach();

    QString output, error;
    const int exitCode = runPowerShellScript(ps.join('\n'), &output, &error);
    if (exitCode != 0)
        return {false, output + "\n" + error};

    QString result;
    for (const QString &line : output.split('\n')) {
        if (line.startsWith("RESULT|"))
            result = line.mid(7).trimmed();
    }
    if (opt.mount == NewVolumeOptions::Folder)
        return {true, folder};
    if (opt.mount == NewVolumeOptions::NoLetter)
        return {true, QString()};
    if (result.isEmpty() || result[0].unicode() == 0)
        return {false, QStringLiteral(
            "The volume was created but did not mount with a drive letter.\n") + output};
    return {true, QStringLiteral("%1:").arg(result[0])};
    #else
    Q_UNUSED(opt);
    return {false, "Not supported on this OS."};
    #endif
}

// -----------------------------------------------------------------------------
// WMI Implementation
// -----------------------------------------------------------------------------

// Initialises COM and connects to the ROOT\CIMV2 WMI namespace (idempotent).
bool HardwareModel::initWmi() {
    #ifdef Q_OS_WIN
    if (wmiInitialized) return true;

    HRESULT hres;

    // Initialize COM.
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres)) {
        if (hres == RPC_E_CHANGED_MODE) {
            CrashHandler::log("CoInitializeEx: RPC_E_CHANGED_MODE (COM already initialized with different mode). Continuing...");
            comInitialized = false; // We didn't init it, so we shouldn't uninit it
        } else {
            CrashHandler::log(QString("CoInitializeEx failed: 0x%1").arg((unsigned long)hres, 0, 16));
            // Try to proceed anyway if it's S_FALSE (already initialized)
            if (hres == S_FALSE) {
                comInitialized = true; // We called it successfully, just redundant
            } else {
                // Fatal error? Usually we can try to proceed if it's just already init
                // But if it failed for other reasons, we might fail later.
            }
        }
    } else {
        comInitialized = true;
    }

    // Initialize Security
    hres = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL
    );

    if (FAILED(hres)) {
        if (hres == RPC_E_TOO_LATE) {
             // Security already set, this is fine
        } else {
             CrashHandler::log(QString("CoInitializeSecurity failed: 0x%1").arg((unsigned long)hres, 0, 16));
             // Don't return false here, as it might be already set by another lib
        }
    }

    IWbemLocator *pLoc = nullptr;
    // Obtain the initial locator to WMI
    hres = CoCreateInstance(
        CLSID_WbemLocator, 0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID *) &pLoc
    );

    if (FAILED(hres)) {
        CrashHandler::log(QString("CoCreateInstance CLSID_WbemLocator failed: 0x%1").arg((unsigned long)hres, 0, 16));
        return false;
    }
    m_pLoc = pLoc;

    IWbemServices *pSvc = nullptr;
    // Connect to WMI through the IWbemLocator::ConnectServer method
    BSTR bstrNamespace = SysAllocString(L"ROOT\\CIMV2");
    hres = pLoc->ConnectServer(
        bstrNamespace,
        NULL, NULL, 0, 0, 0, 0, &pSvc
    );
    SysFreeString(bstrNamespace);

    if (FAILED(hres)) {
        CrashHandler::log(QString("ConnectServer failed: 0x%1").arg((unsigned long)hres, 0, 16));
        pLoc->Release();
        m_pLoc = nullptr;
        return false;
    }
    m_pSvc = pSvc;

    // Set security levels on the proxy
    hres = CoSetProxyBlanket(
        pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE
    );

    if (FAILED(hres)) {
        CrashHandler::log(QString("CoSetProxyBlanket failed: 0x%1").arg((unsigned long)hres, 0, 16));
        pSvc->Release();
        pLoc->Release();
        m_pSvc = nullptr;
        m_pLoc = nullptr;
        return false;
    }

    wmiInitialized = true;
    return true;
    #else
    return false;
    #endif
}

// Releases the WMI services/locator and uninitialises COM if we initialised it.
void HardwareModel::cleanupWmi() {
    #ifdef Q_OS_WIN
    if (m_pSvc) {
        static_cast<IWbemServices*>(m_pSvc)->Release();
        m_pSvc = nullptr;
    }
    if (m_pLoc) {
        static_cast<IWbemLocator*>(m_pLoc)->Release();
        m_pLoc = nullptr;
    }

    wmiInitialized = false;

    if (comInitialized) {
        CoUninitialize();
        comInitialized = false;
    }
    #endif
}

// Runs a WQL query and returns one property map per result row (values as QVariant).
QList<QMap<QString, QVariant>> HardwareModel::queryWmi(const QString &query) {
    QList<QMap<QString, QVariant>> results;
    #ifdef Q_OS_WIN
    if (!wmiInitialized && !initWmi()) return results;

    IWbemServices *pSvc = static_cast<IWbemServices*>(m_pSvc);
    IEnumWbemClassObject* pEnumerator = NULL;

    BSTR bstrWQL = SysAllocString(L"WQL");
    BSTR bstrQuery = SysAllocString(query.toStdWString().c_str());

    HRESULT hres = pSvc->ExecQuery(
        bstrWQL,
        bstrQuery,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL,
        &pEnumerator
    );

    SysFreeString(bstrWQL);
    SysFreeString(bstrQuery);

    if (FAILED(hres)) {
        CrashHandler::log("ExecQuery failed for: " + query + " HRESULT: " + QString::number((unsigned long)hres, 16));
        return results;
    }

    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;

    while (pEnumerator) {
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (0 == uReturn) break;

        QMap<QString, QVariant> map;
        VARIANT vtProp;

        // Enumerate properties
        hres = pclsObj->BeginEnumeration(WBEM_FLAG_NONSYSTEM_ONLY);
        BSTR strName = NULL;
        while (pclsObj->Next(0, &strName, &vtProp, 0, 0) == WBEM_S_NO_ERROR) {
            QString key = QString::fromWCharArray(strName);

            // Convert VARIANT to QVariant
            if (vtProp.vt == VT_BSTR) {
                map[key] = QString::fromWCharArray(vtProp.bstrVal);
            } else if (vtProp.vt == VT_I8) {
                map[key] = QVariant::fromValue<qint64>(vtProp.llVal);
            } else if (vtProp.vt == VT_UI8) {
                ULONGLONG u = vtProp.ullVal;
                const qint64 i64max = (std::numeric_limits<qint64>::max)();
                qint64 n = (u > static_cast<ULONGLONG>(i64max))
                    ? i64max
                    : static_cast<qint64>(u);
                map[key] = QVariant::fromValue<qint64>(n);
            } else if (vtProp.vt == VT_I4 || vtProp.vt == VT_UI4 || vtProp.vt == VT_INT || vtProp.vt == VT_UINT) {
                map[key] = (int)vtProp.intVal; // Simplified
            } else if (vtProp.vt == VT_BOOL) {
                map[key] = (bool)(vtProp.boolVal != 0);
            } else if (vtProp.vt == VT_NULL) {
                map[key] = "N/A";
            } else {
                // Fallback for other types, try to convert to string if possible or ignore
                // For simplicity in this port, we handle strings and ints mostly
                 VariantChangeType(&vtProp, &vtProp, 0, VT_BSTR);
                 if (vtProp.vt == VT_BSTR) {
                     map[key] = QString::fromWCharArray(vtProp.bstrVal);
                 }
            }
            VariantClear(&vtProp);
            SysFreeString(strName);
        }
        pclsObj->EndEnumeration();

        results.append(map);
        pclsObj->Release();
    }
    pEnumerator->Release();
    #endif
    return results;
}

// -----------------------------------------------------------------------------
// Main Logic
// -----------------------------------------------------------------------------

// Starts `powercfg /batteryreport` in the background (parsed later by getBatteryDetails).
void HardwareModel::startBatteryReportAsync() {
    #ifdef Q_OS_WIN
    m_batteryReportPath = QDir::temp().filePath("battery-report.html");
    QFile::remove(m_batteryReportPath);
    m_batteryReportProc = new QProcess(this);
    m_batteryReportProc->start("powercfg",
        QStringList() << "/batteryreport" << "/output" << m_batteryReportPath);
    #endif
}

// Starts the slow PowerShell script that lists devices with driver problems; the
// result is read afterwards with readDriverResult().
void HardwareModel::startDriverCheckAsync() {
    #ifdef Q_OS_WIN
    m_driverPath = QDir::temp().filePath("uqr_driver_check.txt");
    m_encryptionPath = QDir::temp().filePath("uqr_encryption.txt");
    QFile::remove(m_driverPath);
    QFile::remove(m_encryptionPath);

    QString scriptPath = QDir::temp().filePath("uqr_driver_check.ps1");
    QString ps = QString(R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$items = Get-CimInstance -Query "SELECT Name,ConfigManagerErrorCode FROM Win32_PnPEntity WHERE ConfigManagerErrorCode <> 0"
$lines = foreach ($i in $items) { "$($i.Name)|$($i.ConfigManagerErrorCode)" }
Set-Content -Path '%1' -Value $lines -Encoding UTF8
)PS").arg(m_driverPath);

    QFile f(scriptPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(&f) << ps;
        f.close();
        m_driverProc = new QProcess(this);
        m_driverProc->start("powershell", QStringList()
            << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-File" << scriptPath);
    }
    #endif
}

// Starts a fast background probe of the BitLocker/device-encryption state.
void HardwareModel::startEncryptionAsync() {
    #ifdef Q_OS_WIN
    m_encryptionPath = QDir::temp().filePath("uqr_encryption.txt");
    QFile::remove(m_encryptionPath);

    QString scriptPath = QDir::temp().filePath("uqr_encryption.ps1");
    // Win32_EncryptableVolume.ProtectionStatus is language-independent (1 = on).
    QString ps = QString(R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$enc = 'Unknown'
try {
    $sd = $env:SystemDrive
    $v = Get-CimInstance -Namespace 'root\cimv2\security\microsoftvolumeencryption' -ClassName Win32_EncryptableVolume -Filter "DriveLetter='$sd'" -ErrorAction Stop
    if ($v) {
        if ($v.ProtectionStatus -eq 1) { $enc = 'Encrypted' } else { $enc = 'NotEncrypted' }
    }
} catch { }
Set-Content -Path '%1' -Value $enc -Encoding ASCII
)PS").arg(m_encryptionPath);

    QFile f(scriptPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream(&f) << ps;
        f.close();
        m_encProc = new QProcess(this);
        m_encProc->start("powershell", QStringList()
            << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-File" << scriptPath);
    }
    #endif
}

// Waits for the driver-check process, reads its output file and returns a
// "=== Drivers ===" text block (devices with problems) for the log.
QString HardwareModel::readDriverResult() {
    QStringList lines;
    lines << "=== Drivers ===";
    QStringList problem;
    #ifdef Q_OS_WIN
    if (m_driverProc) {
        m_driverProc->waitForFinished(-1);
        QFile df(m_driverPath);
        if (df.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(df.readAll());
            df.close();
            for (const QString &line : content.split('\n', Qt::SkipEmptyParts))
                problem << line.trimmed();
        }
    }
    #endif
    if (problem.isEmpty()) {
        lines << "All drivers installed correctly.";
    } else {
        for (const QString &drv : problem) {
            const QStringList parts = drv.split('|');
            lines << "Device       : " + parts.value(0);
            lines << "Error Code   : " + parts.value(1);
            lines << "Status       : Driver missing or malfunctioning";
            lines << "----------------------------------------";
        }
    }
    return lines.join("\n");
}

// Storage-only refresh (disk hot-plug): init WMI, read disks, emit storageReady.
void HardwareModel::runStorageScan() {
    CrashHandler::log("Starting storage-only refresh...");
    if (!initWmi()) {
        CrashHandler::log("WMI Init failed (storage refresh)");
        emit error("Failed to initialize WMI");
        return;
    }
    QElapsedTimer t;
    t.start();
    QList<StorageDisk> storage = getStorageDetails();
    CrashHandler::log(QString("Storage-only refresh in %1 ms").arg(t.elapsed()));
    cleanupWmi();
    emit storageReady(storage);
}

// Full scan, executed on the worker thread. Order: start background probes ->
// Windows/CPU/GPU/RAM info (emits cardReady per stage) -> storage -> motherboard
// -> battery -> finished(); finally reports the driver check. Any exception is
// converted into the error() signal.
void HardwareModel::runScan() {
    QElapsedTimer scanTimer;
    scanTimer.start();
    CrashHandler::log("Starting hardware scan...");
    emit progress(0, 12, "Initializing WMI...");

    if (!initWmi()) {
        CrashHandler::log("WMI Init failed");
        emit error("Failed to initialize WMI");
        return;
    }

    // Launch background probes now so they run in parallel with the WMI queries.
    // Encryption is fast (needed for the System card); the driver check is slow
    // and its result is collected only AFTER everything else is emitted.
    startBatteryReportAsync();
    startEncryptionAsync();
    startDriverCheckAsync();

    try {
        QElapsedTimer stepTimer;

        SystemSummaryData overview;
        stepTimer.start();
        QString summary = getWindowsInfo([this](int c, int t, QString l){
            emit progress(c, t, l);
        }, overview);
        CrashHandler::log(QString("Read Windows info in %1 ms").arg(stepTimer.elapsed()));

        emit progress(10, 12, "Reading storage details...");
        stepTimer.start();
        QList<StorageDisk> storage = getStorageDetails();
        CrashHandler::log(QString("Read storage details in %1 ms").arg(stepTimer.elapsed()));
        emit storageReady(storage);

        emit progress(11, 12, "Reading motherboard information...");
        stepTimer.start();
        QMap<QString, QString> mb = getMotherboardInfo();
        CrashHandler::log(QString("Read motherboard info in %1 ms").arg(stepTimer.elapsed()));
        emit motherboardReady(mb);

        emit progress(12, 12, "Reading battery details...");
        stepTimer.start();
        QList<BatteryInfo> battery = getBatteryDetails();
        CrashHandler::log(QString("Read battery details in %1 ms").arg(stepTimer.elapsed()));
        emit batteryReady(battery);

        // Encryption status is read inside getWindowsInfo (alongside activation),
        // so overview already carries it here.

        cleanupWmi();
        emit finished("Windows Hardware Info", summary, storage, mb, battery, overview);

        // Display info is detected by the UI thread *after* the other info has
        // rendered (see MainWindow::detectDisplayInfo), so the user sees the rest
        // of the summary first, then the display card fills in.

        // The driver check runs entirely on its own; collect and report it now
        // that all the other info has already been shown.
        stepTimer.start();
        emit driverResult(readDriverResult());
        CrashHandler::log(QString("Read driver check result in %1 ms").arg(stepTimer.elapsed()));
        CrashHandler::log(QString("Scan finished successfully in %1 ms").arg(scanTimer.elapsed()));
    } catch (const std::exception &e) {
        CrashHandler::log(QString("Exception during scan: %1").arg(e.what()));
        cleanupWmi();
        emit error(e.what());
    } catch (...) {
        CrashHandler::log("Unknown exception during scan");
        cleanupWmi();
        emit error("Unknown error during scan");
    }
}

#ifdef Q_OS_WIN
// ── smart system clone helpers ────────────────────────────

// CRC32 (reflected, poly 0xEDB88320) as required by the GPT spec.
static quint32 gptCrc32(const BYTE *data, qint64 len)
{
    static quint32 table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (qint64 i = 0; i < len; ++i)
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static quint32 le32(const BYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (quint32(p[3]) << 24); }
static quint64 le64(const BYTE *p) { return quint64(le32(p)) | (quint64(le32(p + 4)) << 32); }
static void putLe32(BYTE *p, quint32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }
static void putLe64(BYTE *p, quint64 v) { putLe32(p, quint32(v)); putLe32(p + 4, quint32(v >> 32)); }

struct RecoveryMove {
    qint64 sourceOffset = 0;
    qint64 length = 0;
    qint64 targetOffset = 0;
};

// After cloning a GPT disk onto a target of a different size, the backup GPT
// header/entries must be rewritten at the target's last sectors and both headers'
// pointers + CRCs fixed, otherwise the disk shows up as damaged. No-op for MBR.
// Returns false with *err set only for real failures.
static bool fixGptForTargetSize(HANDLE hTarget, qint64 targetSize, DWORD bps,
                                qint64 lastPartitionEndLba, QString *err)
{
    std::vector<BYTE> hdr(bps, 0);
    LARGE_INTEGER pos;
    DWORD io = 0;

    pos.QuadPart = bps; // LBA 1
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, hdr.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to read primary GPT header from target.";
        return false;
    }
    if (memcmp(hdr.data(), "EFI PART", 8) != 0)
        return true; // MBR disk — nothing to fix

    const quint32 headerSize = le32(hdr.data() + 0x0C);
    const quint64 entriesLba = le64(hdr.data() + 0x48);
    const quint32 numEntries = le32(hdr.data() + 0x50);
    const quint32 entrySize  = le32(hdr.data() + 0x54);
    if (headerSize < 92 || headerSize > bps || numEntries == 0 || entrySize == 0 ||
        quint64(numEntries) * entrySize > 1024 * 1024) {
        *err = "Primary GPT header on target looks corrupt.";
        return false;
    }

    const qint64 entriesBytes = qint64(numEntries) * entrySize;
    const qint64 entriesSectors = (entriesBytes + bps - 1) / bps;
    std::vector<BYTE> entries(entriesSectors * bps, 0);
    pos.QuadPart = qint64(entriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) || io != entries.size()) {
        *err = "Failed to read GPT partition entries from target.";
        return false;
    }

    const quint64 lastLba = quint64(targetSize / bps) - 1;
    const quint64 backupEntriesLba = lastLba - entriesSectors;
    const quint64 lastUsableLba = backupEntriesLba - 1;
    // lastPartitionEndLba is exclusive, while lastUsableLba is inclusive.
    if (qint64(lastUsableLba) + 1 < lastPartitionEndLba) {
        *err = "Target disk is too small to hold all partitions plus the backup GPT.";
        return false;
    }

    const quint32 entriesCrc = gptCrc32(entries.data(), entriesBytes);

    auto finalizeHeader = [&](std::vector<BYTE> &h, quint64 myLba, quint64 altLba, quint64 entLba) {
        putLe64(h.data() + 0x18, myLba);
        putLe64(h.data() + 0x20, altLba);
        putLe64(h.data() + 0x30, lastUsableLba);
        putLe64(h.data() + 0x48, entLba);
        putLe32(h.data() + 0x58, entriesCrc);
        putLe32(h.data() + 0x10, 0);
        putLe32(h.data() + 0x10, gptCrc32(h.data(), headerSize));
    };

    std::vector<BYTE> backupHdr = hdr;
    finalizeHeader(hdr, 1, lastLba, entriesLba);
    finalizeHeader(backupHdr, lastLba, 1, backupEntriesLba);

    pos.QuadPart = qint64(backupEntriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) || io != entries.size()) {
        *err = "Failed to write backup GPT entries.";
        return false;
    }
    pos.QuadPart = qint64(lastLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, backupHdr.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to write backup GPT header.";
        return false;
    }
    pos.QuadPart = bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, hdr.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to update primary GPT header.";
        return false;
    }
    return true;
}

// Give the target a temporary GPT disk GUID while Windows mounts and resizes its
// cloned NTFS volume. The original GUID is restored after the target is offline,
// preserving the identifiers referenced by the cloned BCD store.
static bool setTemporaryGptDiskGuid(HANDLE hTarget, qint64 targetSize, DWORD bps,
                                    qint64 lastPartitionEndLba, QString *err)
{
    std::vector<BYTE> hdr(bps, 0);
    LARGE_INTEGER pos = {};
    pos.QuadPart = bps;
    DWORD io = 0;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, hdr.data(), bps, &io, NULL) || io != bps ||
        memcmp(hdr.data(), "EFI PART", 8) != 0) {
        *err = "The expanded clone mode requires a valid GPT target.";
        return false;
    }
    const quint32 headerSize = le32(hdr.data() + 0x0C);
    const quint64 entriesLba = le64(hdr.data() + 0x48);
    const quint32 entryCount = le32(hdr.data() + 0x50);
    const quint32 entrySize = le32(hdr.data() + 0x54);
    if (headerSize < 92 || headerSize > bps || entryCount == 0 || entrySize < 128 ||
        quint64(entryCount) * entrySize > 1024 * 1024) {
        *err = "The target GPT header is invalid.";
        return false;
    }
    const qint64 entriesBytes = qint64(entryCount) * entrySize;
    const qint64 entriesSectors = (entriesBytes + bps - 1) / bps;
    std::vector<BYTE> entries(entriesSectors * bps, 0);
    pos.QuadPart = qint64(entriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) ||
        io != entries.size()) {
        *err = "Failed to read target GPT entries before expansion.";
        return false;
    }

    // Partition IDs are globally visible to the mount manager. Randomize them
    // along with the disk GUID so the target clone can coexist with the source
    // just long enough for Resize-Partition to grow its NTFS filesystem.
    for (quint32 i = 0; i < entryCount; ++i) {
        BYTE *entry = entries.data() + qint64(i) * entrySize;
        bool used = false;
        for (int b = 0; b < 16; ++b)
            used = used || entry[b] != 0;
        if (!used)
            continue;
        const QByteArray partitionGuid = QUuid::createUuid().toRfc4122();
        memcpy(entry + 16, partitionGuid.constData(), 16);
    }
    const QByteArray randomGuid = QUuid::createUuid().toRfc4122();
    memcpy(hdr.data() + 0x38, randomGuid.constData(), 16);
    putLe32(hdr.data() + 0x58, gptCrc32(entries.data(), entriesBytes));
    putLe32(hdr.data() + 0x10, 0);
    putLe32(hdr.data() + 0x10, gptCrc32(hdr.data(), headerSize));
    pos.QuadPart = qint64(entriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) ||
        io != entries.size()) {
        *err = "Failed to assign temporary GPT partition identities to the target.";
        return false;
    }
    pos.QuadPart = bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, hdr.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to assign a temporary GPT identity to the target.";
        return false;
    }
    return fixGptForTargetSize(hTarget, targetSize, bps, lastPartitionEndLba, err);
}

// Restore each source recovery GPT entry at its new end-of-disk offset and put
// back the source disk GUID after Resize-Partition has expanded the NTFS volume.
// With an empty 'moves' list this only restores the original disk/partition
// GUIDs — used as a best-effort salvage when a later expansion step fails, so
// the target still boots as a WinRE-less clone instead of being left with the
// temporary identity.
static bool restoreRelocatedRecoveryGpt(HANDLE hSource, HANDLE hTarget,
                                        qint64 targetSize, DWORD bps,
                                        qint64 systemOffset,
                                        const QList<RecoveryMove> &moves,
                                        QString *err)
{
    auto readHeaderAndEntries = [&](HANDLE disk, std::vector<BYTE> *hdr,
                                    std::vector<BYTE> *entries,
                                    quint32 *entryCount, quint32 *entrySize) -> bool {
        hdr->assign(bps, 0);
        LARGE_INTEGER pos = {};
        pos.QuadPart = bps;
        DWORD io = 0;
        if (!SetFilePointerEx(disk, pos, nullptr, FILE_BEGIN) ||
            !ReadFile(disk, hdr->data(), bps, &io, NULL) || io != bps ||
            memcmp(hdr->data(), "EFI PART", 8) != 0)
            return false;
        *entryCount = le32(hdr->data() + 0x50);
        *entrySize = le32(hdr->data() + 0x54);
        const quint64 entriesLba = le64(hdr->data() + 0x48);
        if (*entryCount == 0 || *entrySize < 128 ||
            quint64(*entryCount) * *entrySize > 1024 * 1024)
            return false;
        const qint64 bytes = qint64(*entryCount) * *entrySize;
        const qint64 sectors = (bytes + bps - 1) / bps;
        entries->assign(sectors * bps, 0);
        pos.QuadPart = qint64(entriesLba) * bps;
        return SetFilePointerEx(disk, pos, nullptr, FILE_BEGIN) &&
               ReadFile(disk, entries->data(), DWORD(entries->size()), &io, NULL) &&
               io == entries->size();
    };

    std::vector<BYTE> sourceHdr, targetHdr, sourceEntries, targetEntries;
    quint32 sourceCount = 0, sourceEntrySize = 0, targetCount = 0, targetEntrySize = 0;
    if (!readHeaderAndEntries(hSource, &sourceHdr, &sourceEntries, &sourceCount, &sourceEntrySize) ||
        !readHeaderAndEntries(hTarget, &targetHdr, &targetEntries, &targetCount, &targetEntrySize) ||
        sourceCount != targetCount || sourceEntrySize != targetEntrySize) {
        *err = "Failed to read compatible source and target GPT tables.";
        return false;
    }

    bool foundSystem = false;
    for (quint32 i = 0; i < targetCount; ++i) {
        BYTE *entry = targetEntries.data() + qint64(i) * targetEntrySize;
        if (qint64(le64(entry + 32)) * bps == systemOffset) {
            foundSystem = true;
            break;
        }
    }
    if (!foundSystem) {
        *err = "The expanded target system partition could not be found.";
        return false;
    }

    // A GPT entry is in use when its partition type GUID (first 16 bytes) is
    // non-zero.
    auto entryUsed = [](const BYTE *entry) {
        for (int b = 0; b < 16; ++b)
            if (entry[b] != 0)
                return true;
        return false;
    };

    // Put back the original unique partition IDs that the cloned BCD and mount
    // metadata expect. Match by starting offset because the system length changed.
    for (quint32 ti = 0; ti < targetCount; ++ti) {
        BYTE *targetEntry = targetEntries.data() + qint64(ti) * targetEntrySize;
        if (!entryUsed(targetEntry))
            continue;
        const quint64 targetStart = le64(targetEntry + 32);
        for (quint32 si = 0; si < sourceCount; ++si) {
            const BYTE *sourceEntry = sourceEntries.data() + qint64(si) * sourceEntrySize;
            if (le64(sourceEntry + 32) == targetStart) {
                memcpy(targetEntry + 16, sourceEntry + 16, 16);
                break;
            }
        }
    }

    for (const RecoveryMove &move : moves) {
        bool restored = false;
        for (quint32 i = 0; i < sourceCount && !restored; ++i) {
            const BYTE *sourceEntry = sourceEntries.data() + qint64(i) * sourceEntrySize;
            const qint64 sourceStart = qint64(le64(sourceEntry + 32)) * bps;
            const qint64 sourceEnd = (qint64(le64(sourceEntry + 40)) + 1) * bps;
            if (sourceStart != move.sourceOffset || sourceEnd - sourceStart != move.length)
                continue;
            // Prefer the source entry's own slot; if the resize step rewrote
            // the target entry array differently and that slot is now taken,
            // fall back to any free slot instead of clobbering a live entry.
            BYTE *targetEntry = targetEntries.data() + qint64(i) * targetEntrySize;
            if (entryUsed(targetEntry)) {
                targetEntry = nullptr;
                for (quint32 j = 0; j < targetCount && !targetEntry; ++j) {
                    BYTE *candidate = targetEntries.data() + qint64(j) * targetEntrySize;
                    if (!entryUsed(candidate))
                        targetEntry = candidate;
                }
                if (!targetEntry) {
                    *err = "No free GPT slot is left for the relocated recovery partition.";
                    return false;
                }
            }
            memcpy(targetEntry, sourceEntry, targetEntrySize);
            putLe64(targetEntry + 32, quint64(move.targetOffset / bps));
            putLe64(targetEntry + 40, quint64((move.targetOffset + move.length) / bps - 1));
            restored = true;
        }
        if (!restored) {
            *err = "A source recovery GPT entry could not be restored.";
            return false;
        }
    }

    // Restore the disk GUID that the cloned BCD expects.
    memcpy(targetHdr.data() + 0x38, sourceHdr.data() + 0x38, 16);
    const qint64 entriesBytes = qint64(targetCount) * targetEntrySize;
    putLe32(targetHdr.data() + 0x58, gptCrc32(targetEntries.data(), entriesBytes));
    const quint32 headerSize = le32(targetHdr.data() + 0x0C);
    putLe32(targetHdr.data() + 0x10, 0);
    putLe32(targetHdr.data() + 0x10, gptCrc32(targetHdr.data(), headerSize));

    LARGE_INTEGER pos = {};
    DWORD io = 0;
    pos.QuadPart = qint64(le64(targetHdr.data() + 0x48)) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, targetEntries.data(), DWORD(targetEntries.size()), &io, NULL) ||
        io != targetEntries.size()) {
        *err = "Failed to write relocated recovery GPT entries.";
        return false;
    }
    pos.QuadPart = bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, targetHdr.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to restore the target GPT header.";
        return false;
    }

    qint64 lastEnd = 0;
    if (!moves.isEmpty()) {
        const RecoveryMove &lastMove = moves.constLast();
        lastEnd = lastMove.targetOffset + lastMove.length;
    } else {
        for (quint32 i = 0; i < targetCount; ++i) {
            const BYTE *entry = targetEntries.data() + qint64(i) * targetEntrySize;
            if (entryUsed(entry))
                lastEnd = qMax(lastEnd, (qint64(le64(entry + 40)) + 1) * bps);
        }
    }
    return fixGptForTargetSize(hTarget, targetSize, bps, lastEnd / bps, err);
}

// Remove omitted recovery partitions from the copied partition table. Their
// payload is never written, and the space becomes unallocated on the clone.
static bool removePartitionsFromTargetTable(HANDLE hTarget, DWORD bps,
                                             const QList<QPair<qint64, qint64>> &omitted,
                                             QString *err)
{
    if (omitted.isEmpty())
        return true;

    std::vector<BYTE> sector(bps, 0);
    LARGE_INTEGER pos = {};
    DWORD io = 0;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, sector.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to read the target partition table.";
        return false;
    }

    // Legacy MBR: recovery partitions use type 0x27.
    if (bps >= 512 && memcmp(sector.data() + 510, "\x55\xAA", 2) == 0) {
        bool changed = false;
        for (int i = 0; i < 4; ++i) {
            BYTE *entry = sector.data() + 446 + i * 16;
            const qint64 start = qint64(le32(entry + 8)) * bps;
            const qint64 length = qint64(le32(entry + 12)) * bps;
            for (const auto &range : omitted) {
                if (entry[4] == 0x27 && start == range.first && length == range.second) {
                    memset(entry, 0, 16);
                    changed = true;
                }
            }
        }
        if (changed) {
            pos.QuadPart = 0;
            if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
                !WriteFile(hTarget, sector.data(), bps, &io, NULL) || io != bps) {
                *err = "Failed to update the target MBR partition table.";
                return false;
            }
        }
    }

    // GPT: clear matching entries and update the primary header CRCs. The
    // normal GPT finalizer will create a matching backup table afterward.
    pos.QuadPart = bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, sector.data(), bps, &io, NULL) || io != bps)
        return true; // MBR-only disk
    if (memcmp(sector.data(), "EFI PART", 8) != 0)
        return true;

    const quint32 headerSize = le32(sector.data() + 0x0C);
    const quint64 entriesLba = le64(sector.data() + 0x48);
    const quint32 numEntries = le32(sector.data() + 0x50);
    const quint32 entrySize = le32(sector.data() + 0x54);
    if (headerSize < 92 || headerSize > bps || entrySize < 48 ||
        numEntries == 0 || quint64(numEntries) * entrySize > 1024 * 1024) {
        *err = "Target GPT header looks corrupt while removing recovery partitions.";
        return false;
    }
    const qint64 entriesBytes = qint64(numEntries) * entrySize;
    const qint64 entriesSectors = (entriesBytes + bps - 1) / bps;
    std::vector<BYTE> entries(entriesSectors * bps, 0);
    pos.QuadPart = qint64(entriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !ReadFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) || io != entries.size()) {
        *err = "Failed to read target GPT entries.";
        return false;
    }
    for (quint32 i = 0; i < numEntries; ++i) {
        BYTE *entry = entries.data() + qint64(i) * entrySize;
        const qint64 start = qint64(le64(entry + 32)) * bps;
        const qint64 end = (qint64(le64(entry + 40)) + 1) * bps;
        for (const auto &range : omitted) {
            if (start == range.first && end - start == range.second)
                memset(entry, 0, entrySize);
        }
    }
    putLe32(sector.data() + 0x58, gptCrc32(entries.data(), entriesBytes));
    putLe32(sector.data() + 0x10, 0);
    putLe32(sector.data() + 0x10, gptCrc32(sector.data(), headerSize));
    pos.QuadPart = qint64(entriesLba) * bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, entries.data(), DWORD(entries.size()), &io, NULL) || io != entries.size()) {
        *err = "Failed to write target GPT entries.";
        return false;
    }
    pos.QuadPart = bps;
    if (!SetFilePointerEx(hTarget, pos, nullptr, FILE_BEGIN) ||
        !WriteFile(hTarget, sector.data(), bps, &io, NULL) || io != bps) {
        *err = "Failed to update the target GPT header.";
        return false;
    }
    return true;
}

// Creates a VSS snapshot of the given volume ("C:") so the clone reads a frozen,
// filesystem-consistent image while Windows keeps running. Returns the shadow
// device path (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN) and the shadow ID
// for later deletion. Uses PowerShell/CIM like the app's other probes.
static bool createVssSnapshot(const QString &driveLetter, QString *devicePath, QString *shadowId)
{
    const QString script = QString(
        "$r = Invoke-CimMethod -ClassName Win32_ShadowCopy -Namespace root/cimv2 -MethodName Create "
        "-Arguments @{ Volume = '%1\\'; Context = 'ClientAccessible' }; "
        "if ($r.ReturnValue -eq 0) { "
        "  $s = Get-CimInstance -ClassName Win32_ShadowCopy -Namespace root/cimv2 | Where-Object { $_.ID -eq $r.ShadowID }; "
        "  Write-Output ($r.ShadowID + '|' + $s.DeviceObject) "
        "}").arg(driveLetter);

    QProcess p;
    p.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << script);
    if (!p.waitForFinished(90000)) {
        p.kill();
        return false;
    }
    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    const int sep = out.indexOf('|');
    if (sep <= 0)
        return false;
    *shadowId = out.left(sep).trimmed();
    *devicePath = out.mid(sep + 1).trimmed();
    return !devicePath->isEmpty() && devicePath->startsWith("\\\\?\\GLOBALROOT", Qt::CaseInsensitive);
}

static void deleteVssSnapshot(const QString &shadowId)
{
    if (shadowId.isEmpty())
        return;
    // Delete through the SAME WMI provider that created it (Remove-CimInstance
    // matches the exact shadow by ID and is far more reliable than vssadmin,
    // which frequently fails to match a WMI-created ClientAccessible shadow).
    // If the snapshot is not removed, its copy-on-write storage lingers in
    // System Volume Information and counts as used clusters — so every clone
    // would then copy progressively MORE data than the last.
    const QString script = QString(
        "$s = Get-CimInstance -ClassName Win32_ShadowCopy -Namespace root/cimv2 | "
        "Where-Object { $_.ID -eq '%1' }; "
        "if ($s) { Remove-CimInstance -InputObject $s; Write-Output 'DELETED' } "
        "else { Write-Output 'GONE' }").arg(shadowId);
    QProcess p;
    p.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                        << "-Command" << script);
    p.waitForFinished(60000);
    QString out = QString::fromLocal8Bit(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();

    if (!out.contains("DELETED") && !out.contains("GONE")) {
        // Fallback to vssadmin by ID.
        QProcess v;
        v.start("vssadmin", QStringList() << "delete" << "shadows" << ("/shadow=" + shadowId) << "/quiet");
        v.waitForFinished(30000);
        out += " | vssadmin: " + QString::fromLocal8Bit(
            v.readAllStandardOutput() + v.readAllStandardError()).trimmed();
    }
    CrashHandler::log("VSS snapshot cleanup (" + shadowId + "): " + out);
}

// Best-effort: make sure the services a VSS snapshot needs ("Volume Shadow Copy"
// and "Microsoft Software Shadow Copy Provider") are not disabled and are
// running, so createVssSnapshot() does not fail on machines where they were
// turned off. Non-fatal — the snapshot attempt itself reports real failure.
static void ensureVssServicesRunning()
{
    const QString script =
        "foreach ($n in 'VSS','swprv') { try { "
        "$s = Get-Service -Name $n -ErrorAction Stop; "
        "if ($s.StartType -eq 'Disabled') { Set-Service -Name $n -StartupType Manual }; "
        "if ($s.Status -ne 'Running') { Start-Service -Name $n } } catch {} }";
    QProcess p;
    p.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                        << "-Command" << script);
    p.waitForFinished(30000);
}

// ── shrink-to-fit safety net ──────────────────────────────────────────────
// Shrinking the live source C: is the riskiest part of a smaller-target clone:
// if the app dies midway, C: stays shrunk. A small marker file records what to
// undo (original size, hibernation state) so the next launch can repair it.
static QString cloneMarkerPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + "/QuickReports";
    QDir().mkpath(dir);
    return dir + "/pending_clone_restore.txt";
}

static void writeCloneMarker(const QString &driveLetter, qint64 origBytes, bool hibernateWasOn)
{
    QFile f(cloneMarkerPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(QString("%1|%2|%3\n").arg(driveLetter).arg(origBytes).arg(hibernateWasOn ? 1 : 0).toUtf8());
}

static bool hibernationEnabled()
{
    QSettings s("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Power", QSettings::NativeFormat);
    return s.value("HibernateEnabled", 0).toInt() != 0;
}

// Frees what Windows' shrink floor (Get-PartitionSupportedSize SizeMin) is
// pinned by and that we can safely release without a reboot: the hibernation
// file and System Restore / shadow-copy storage. The page file can only be
// moved with a reboot, so it is reported, not touched. Returns a log line.
static QString reclaimShrinkSpace(const QString &sysDrive, bool *hibernateDisabledByUs)
{
    QString report;
    if (hibernationEnabled()) {
        QProcess::execute("powercfg", QStringList() << "/h" << "off");
        *hibernateDisabledByUs = true;
        report += "hibernation file removed; ";
    }
    QProcess v;
    v.start("vssadmin", QStringList() << "delete" << "shadows" << ("/for=" + sysDrive) << "/all" << "/quiet");
    v.waitForFinished(120000);
    report += "restore points / shadow copies deleted; ";
    // Re-trim and consolidate free space so freshly freed clusters at the end
    // of the volume become usable by the shrink.
    QProcess t;
    t.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command"
            << QString("Optimize-Volume -DriveLetter %1 -ReTrim -ErrorAction SilentlyContinue").arg(sysDrive.at(0)));
    t.waitForFinished(120000);
    return report;
}
#endif // Q_OS_WIN

QString HardwareModel::recoverInterruptedClone() {
    #ifdef Q_OS_WIN
    QFile f(cloneMarkerPath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    const QStringList parts = QString::fromUtf8(f.readAll()).trimmed().split('|');
    f.close();
    if (parts.size() < 3)
        return QString();
    const QString drive = parts[0].left(1);
    const qint64 orig = parts[1].toLongLong();
    const bool hibernate = parts[2] == "1";

    QString msg = "Recovering from an interrupted clone: ";
    if (orig > 0 && drive.size() == 1) {
        // Grow back to the original size; fall back to the largest possible.
        const QString script = QString(
            "$ErrorActionPreference='Stop'; "
            // Only grow when the volume is actually still smaller than before; a
            // marker left by a run that died before shrinking must not absorb
            // free space that was there originally.
            "if ((Get-Partition -DriveLetter %1).Size -lt %2) { "
            "try { Resize-Partition -DriveLetter %1 -Size %2 } "
            "catch { $s=Get-PartitionSupportedSize -DriveLetter %1; Resize-Partition -DriveLetter %1 -Size $s.SizeMax } }")
            .arg(drive).arg(orig);
        QProcess ps;
        ps.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                             << "-Command" << script);
        ps.waitForFinished(600000);
        msg += ps.exitCode() == 0 ? QString("%1: restored to full size. ").arg(drive)
                                  : QString("could not restore %1: (%2). ")
                                        .arg(drive, QString::fromLocal8Bit(ps.readAllStandardError()).trimmed());
    }
    if (hibernate && !hibernationEnabled()) {
        QProcess::execute("powercfg", QStringList() << "/h" << "on");
        msg += "hibernation re-enabled. ";
    }
    f.remove();
    CrashHandler::log(msg);
    return msg;
    #else
    return QString();
    #endif
}

// Clone entry points (see runSystemCloneMode for the mode numbers):
//   0 = keep recovery partition   1 = without recovery
//   2 = expand to fill larger target   3 = shrink to fit smaller target
void HardwareModel::runSystemClone(int sourceIdx, int targetIdx) {
    try {
        runSystemCloneMode(sourceIdx, targetIdx, 0);
    } catch (const std::exception &e) {
        const QString message = QString("System clone stopped: %1").arg(e.what());
        CrashHandler::log(message);
        emit cloneFinished(false, message);
    } catch (...) {
        CrashHandler::log("System clone stopped because of an unexpected error.");
        emit cloneFinished(false, "System clone stopped because of an unexpected error.");
    }
}

void HardwareModel::runSystemCloneWithoutRecovery(int sourceIdx, int targetIdx) {
    try {
        runSystemCloneMode(sourceIdx, targetIdx, 1);
    } catch (const std::exception &e) {
        const QString message = QString("System clone stopped: %1").arg(e.what());
        CrashHandler::log(message);
        emit cloneFinished(false, message);
    } catch (...) {
        CrashHandler::log("System clone stopped because of an unexpected error.");
        emit cloneFinished(false, "System clone stopped because of an unexpected error.");
    }
}

void HardwareModel::runSystemCloneExpanded(int sourceIdx, int targetIdx) {
    try {
        runSystemCloneMode(sourceIdx, targetIdx, 2);
    } catch (const std::exception &e) {
        const QString message = QString("System clone stopped: %1").arg(e.what());
        CrashHandler::log(message);
        emit cloneFinished(false, message);
    } catch (...) {
        CrashHandler::log("System clone stopped because of an unexpected error.");
        emit cloneFinished(false, "System clone stopped because of an unexpected error.");
    }
}

void HardwareModel::runSystemCloneShrink(int sourceIdx, int targetIdx) {
    try {
        runSystemCloneMode(sourceIdx, targetIdx, 3);
    } catch (const std::exception &e) {
        const QString message = QString("System clone stopped: %1").arg(e.what());
        CrashHandler::log(message);
        emit cloneFinished(false, message);
    } catch (...) {
        CrashHandler::log("System clone stopped because of an unexpected error.");
        emit cloneFinished(false, "System clone stopped because of an unexpected error.");
    }
}

// Core of the cloning feature. Flow:
//   validate (admin, different disks) -> locate source system volume -> check
//   geometry -> (shrink mode: shrink C: first) -> read source partition table ->
//   create VSS snapshot -> prepare target (clean + copy partition table) -> copy
//   data block by block with progress/cancel -> fix backup GPT -> restore GUIDs ->
//   (expand modes: grow NTFS, relocate recovery) -> re-enable WinRE / cleanup.
// Emits cloneProgress*/cloneStatus along the way and cloneFinished at the end.
void HardwareModel::runSystemCloneMode(int sourceIdx, int targetIdx, int cloneMode) {
    #ifdef Q_OS_WIN
    bool includeRecovery = cloneMode == 0;
    bool relocateRecovery = cloneMode == 2;    // shrink mode sets this after WinRE detection
    bool shrinkToFit = cloneMode == 3;         // large disk → smaller disk
    if (cloneMode < 0 || cloneMode > 3) {
        emit cloneFinished(false, "Unknown system clone mode.");
        return;
    }
    m_cloneCancelRequested.store(false);
    if (!isUserAdmin()) {
        emit cloneFinished(false, "Administrator privileges are required.");
        return;
    }
    if (sourceIdx == targetIdx) {
        emit cloneFinished(false, "Source and target must be different disks.");
        return;
    }

    CrashHandler::log(QString("System clone started: disk %1 -> disk %2, mode=%3")
                          .arg(sourceIdx).arg(targetIdx).arg(cloneMode));

    // ── resolve the system volume's disk/offset via IOCTLs (no WMI needed) ──
    QString sysDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
    if (sysDrive.size() < 2 || sysDrive[1] != ':')
        sysDrive = "C:";
    sysDrive = sysDrive.left(2).toUpper();

    qint64 sysVolOffset = -1;
    {
        HANDLE hVol = CreateFileW((LPCWSTR)(QString("\\\\.\\%1").arg(sysDrive)).toStdWString().c_str(),
                                  0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hVol != INVALID_HANDLE_VALUE) {
            BYTE extBuf[sizeof(VOLUME_DISK_EXTENTS) + 4 * sizeof(DISK_EXTENT)] = {};
            DWORD io = 0;
            if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0,
                                extBuf, sizeof(extBuf), &io, NULL)) {
                auto *ext = reinterpret_cast<VOLUME_DISK_EXTENTS*>(extBuf);
                if (ext->NumberOfDiskExtents >= 1 &&
                    int(ext->Extents[0].DiskNumber) == sourceIdx) {
                    sysVolOffset = ext->Extents[0].StartingOffset.QuadPart;
                }
            }
            CloseHandle(hVol);
        }
    }
    if (sysVolOffset < 0) {
        emit cloneFinished(false, QString("Disk %1 does not host the running system volume (%2).").arg(sourceIdx).arg(sysDrive));
        return;
    }

    // Clearing the target drive is a separate, manual step — use the "Clear
    // Drive" action first if the target still has partitions. The clone writes a
    // fresh partition table over whatever is there, but it no longer wipes the
    // disk automatically.
    emit cloneStatus("clone_stage_prepare");

    QString sourcePath = QString("\\\\.\\PhysicalDrive%1").arg(sourceIdx);
    QString targetPath = QString("\\\\.\\PhysicalDrive%1").arg(targetIdx);

    HANDLE hSource = CreateFileW((LPCWSTR)sourcePath.toStdWString().c_str(),
                                 GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    HANDLE hTarget = CreateFileW((LPCWSTR)targetPath.toStdWString().c_str(),
                                 GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hSource == INVALID_HANDLE_VALUE || hTarget == INVALID_HANDLE_VALUE) {
        if (hSource != INVALID_HANDLE_VALUE) CloseHandle(hSource);
        if (hTarget != INVALID_HANDLE_VALUE) CloseHandle(hTarget);
        emit cloneFinished(false, "Could not open source or target disk. Ensure you are Admin and no other disk utility is running.");
        return;
    }

    HANDLE hShadow = INVALID_HANDLE_VALUE;
    // Dedicated write handle for the bulk copy. Opened FILE_FLAG_NO_BUFFERING so
    // transfers go straight to the device instead of through the Windows cache
    // manager (which roughly halves sustained write speed to USB bridges). The
    // small GPT edits keep using the buffered hTarget. alignedBuf is the
    // page-aligned transfer buffer that unbuffered I/O requires.
    HANDLE hTargetCopy = INVALID_HANDLE_VALUE;
    void *alignedBuf = nullptr;
    QString shadowId;
    // For shrink-to-fit clones: once the source system volume has been shrunk,
    // this holds the original byte size so cleanup() can grow it back on ANY
    // exit path (success, failure, or cancel). -1 means nothing to restore.
    qint64 srcRestoreSizeBytes = -1;
    bool hibernateDisabledByUs = false;   // turned off to lower the shrink floor; restored in cleanup()
    auto cleanup = [&]() {
        if (hShadow != INVALID_HANDLE_VALUE) { CloseHandle(hShadow); hShadow = INVALID_HANDLE_VALUE; }
        if (hSource != INVALID_HANDLE_VALUE) { CloseHandle(hSource); hSource = INVALID_HANDLE_VALUE; }
        if (hTargetCopy != INVALID_HANDLE_VALUE) { CloseHandle(hTargetCopy); hTargetCopy = INVALID_HANDLE_VALUE; }
        if (hTarget != INVALID_HANDLE_VALUE) { CloseHandle(hTarget); hTarget = INVALID_HANDLE_VALUE; }
        if (alignedBuf) { VirtualFree(alignedBuf, 0, MEM_RELEASE); alignedBuf = nullptr; }
        deleteVssSnapshot(shadowId);
        shadowId.clear();
        // Grow the source system volume back to its original size (VSS snapshot
        // already removed above, so nothing blocks the resize).
        if (srcRestoreSizeBytes > 0) {
            emit cloneStatus("clone_stage_restore");
            const QString restore = QString("$ErrorActionPreference='Stop'; "
                "Resize-Partition -DriveLetter %1 -Size %2")
                .arg(sysDrive.at(0)).arg(srcRestoreSizeBytes);
            QProcess rp;
            rp.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                                 << "-Command" << restore);
            rp.waitForFinished(600000);
            const QString rout = QString::fromLocal8Bit(
                rp.readAllStandardOutput() + "\n" + rp.readAllStandardError()).trimmed();
            CrashHandler::log(QString("Shrink clone: restored source %1 to %2 bytes. %3")
                                  .arg(sysDrive).arg(srcRestoreSizeBytes).arg(rout));
            srcRestoreSizeBytes = -1; // restore only once
        }
        if (hibernateDisabledByUs) {
            QProcess::execute("powercfg", QStringList() << "/h" << "on");
            hibernateDisabledByUs = false;
        }
        QFile::remove(cloneMarkerPath()); // everything undone — nothing to recover on next launch
    };
    // Filled in once the size check below runs; appended to every failure
    // message so the user sees the same "why" that's in the log, not just a
    // bare error — a size mismatch/limit should never be reported without the
    // actual numbers that caused it.
    QString sizeCheckDiag;
    auto fail = [&](const QString &msg) {
        const QString full = sizeCheckDiag.isEmpty() ? msg : msg + "\n\n" + sizeCheckDiag;
        CrashHandler::log("System clone failed: " + full);
        cleanup();
        emit cloneFinished(false, full);
    };

    DWORD io = 0;
    DeviceIoControl(hTarget, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &io, NULL);

    // ── geometry: sizes + sector size must match between the two disks ──
    auto diskInfo = [&](HANDLE h, qint64 *size, DWORD *bytesPerSector) -> bool {
        DISK_GEOMETRY_EX geo = {};
        DWORD ret = 0;
        if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof(geo), &ret, NULL))
            return false;
        *size = geo.DiskSize.QuadPart;
        *bytesPerSector = geo.Geometry.BytesPerSector;
        return true;
    };
    qint64 sourceSize = 0, targetSize = 0;
    DWORD srcBps = 0, tgtBps = 0;
    if (!diskInfo(hSource, &sourceSize, &srcBps)) { fail("Failed to read source disk geometry."); return; }
    if (!diskInfo(hTarget, &targetSize, &tgtBps)) { fail("Failed to read target disk geometry."); return; }
    if (srcBps != tgtBps) {
        fail(QString("Sector size mismatch: source %1 B, target %2 B. A sector clone between them would not boot.").arg(srcBps).arg(tgtBps));
        return;
    }
    const DWORD bps = srcBps ? srcBps : 512;

    // ── log the real achievable sizes up front, for every clone mode ──
    // Not just the shrink-to-fit path: knowing exactly what Windows says the
    // system volume's shrinkable range and current size are — before anything
    // is touched — is what a later debugging session actually needs, whichever
    // mode failed and however far it got. Best-effort: a query failure here
    // never blocks the clone, it just logs "unknown".
    {
        const QString sizeScript = QString(
            "$ErrorActionPreference='Stop';"
            "$p=Get-Partition -DriveLetter %1;"
            "$sup=Get-PartitionSupportedSize -DriveLetter %1;"
            "Write-Output ('CURSIZE='+$p.Size);"
            "Write-Output ('MINSIZE='+$sup.SizeMin);"
            "Write-Output ('MAXSIZE='+$sup.SizeMax)")
            .arg(sysDrive.at(0));
        QProcess sizeProc;
        sizeProc.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                                    << "-Command" << sizeScript);
        sizeProc.waitForFinished(15000);
        const QString sizeOut = QString::fromLocal8Bit(sizeProc.readAllStandardOutput()).trimmed();
        QString curSize = "unknown", minSize = "unknown", maxSize = "unknown";
        for (const QString &line : sizeOut.split('\n')) {
            const QString t = line.trimmed();
            if (t.startsWith("CURSIZE=")) curSize = t.mid(8);
            else if (t.startsWith("MINSIZE=")) minSize = t.mid(8);
            else if (t.startsWith("MAXSIZE=")) maxSize = t.mid(8);
        }
        CrashHandler::log(QString(
            "System clone size check: mode=%1, source disk=%2 B, target disk=%3 B, "
            "system volume %4 starts at offset %5; current=%6 B / shrinkable-min=%7 B / "
            "growable-max=%8 B.")
                .arg(cloneMode).arg(sourceSize).arg(targetSize)
                .arg(sysDrive).arg(sysVolOffset)
                .arg(curSize).arg(minSize).arg(maxSize));

        // Same numbers, human-readable, attached to any failure message below
        // (see `fail`) so the user sees the actual cause, not just "it failed".
        auto gb = [](const QString &bytes) -> QString {
            bool ok = false;
            const double v = bytes.toDouble(&ok);
            return ok ? QString::number(v / 1e9, 'f', 1) + " GB" : "unknown";
        };
        sizeCheckDiag = QString(
            "Diagnostics: source disk %1, target disk %2; system volume %3 is currently %4 "
            "(Windows says it can shrink to at least %5, and grow up to %6).")
                .arg(gb(QString::number(sourceSize)), gb(QString::number(targetSize)),
                     sysDrive, gb(curSize), gb(minSize), gb(maxSize));
    }

    // Safety net for a mis-picked mode: a 1:1 / expand clone can never fit a
    // physically smaller target, so switch to shrink-to-fit (copies used data
    // only, like other cloning tools do) instead of failing with "too small".
    if (!shrinkToFit && targetSize + qint64(1024) * 1024 * 1024 < sourceSize) {
        CrashHandler::log(QString("System clone: mode %1 requested but target (%2 B) is smaller "
                                  "than source (%3 B); switching to shrink-to-fit.")
                              .arg(cloneMode).arg(targetSize).arg(sourceSize));
        cloneMode = 3;
        includeRecovery = false;
        relocateRecovery = false;
        shrinkToFit = true;
    }

    // ── shrink-to-fit: temporarily shrink the source system volume so a larger
    // source disk fits a smaller target (recovery is dropped; the system is the
    // last kept partition). cleanup() grows the source back afterwards. ──
    if (shrinkToFit) {
        ULARGE_INTEGER availToCaller = {}, total = {}, totalFree = {};
        const std::wstring rootPath = (sysDrive + "\\").toStdWString();
        qint64 usedBytes = -1;
        if (GetDiskFreeSpaceExW(rootPath.c_str(), &availToCaller, &total, &totalFree))
            usedBytes = qint64(total.QuadPart) - qint64(totalFree.QuadPart);
        if (usedBytes < 0) { fail("Could not read used space on the system volume."); return; }

        // Preliminary layout scan: total recovery-partition size + whether this
        // system actually has WinRE, so we can reserve room for a relocated
        // recovery partition (kept only when the source has WinRE).
        qint64 recoveryTotal = 0;
        {
            std::vector<BYTE> buf(65536, 0);
            DWORD ret = 0;
            if (DeviceIoControl(hSource, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                                buf.data(), DWORD(buf.size()), &ret, NULL)) {
                auto *layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
                static const GUID recGuid =
                    {0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}};
                for (DWORD i = 0; i < layout->PartitionCount; ++i) {
                    PARTITION_INFORMATION_EX &pe = layout->PartitionEntry[i];
                    if (pe.PartitionLength.QuadPart <= 0)
                        continue;
                    const bool isRec =
                        (pe.PartitionStyle == PARTITION_STYLE_GPT && IsEqualGUID(pe.Gpt.PartitionType, recGuid)) ||
                        (pe.PartitionStyle == PARTITION_STYLE_MBR && pe.Mbr.PartitionType == 0x27);
                    if (isRec)
                        recoveryTotal += pe.PartitionLength.QuadPart;
                }
            }
        }
        const bool keepRecovery = readWinReInfo(nullptr) && recoveryTotal > 0;

        // Largest system size that still fits the target. Reserve the backup
        // GPT, and — when keeping WinRE — the relocated recovery partition plus
        // alignment. A margin keeps sTarget below the eventual target C: size so
        // the on-target resize is always a (safe) grow.
        const qint64 alignment = 1024LL * 1024;
        const qint64 gptReserve = 64LL * 1024 * 1024;
        const qint64 margin = 256LL * 1024 * 1024;
        const qint64 recoveryReserve = keepRecovery ? (recoveryTotal + 4 * alignment) : 0;
        qint64 sTarget = ((targetSize - sysVolOffset - recoveryReserve - gptReserve - margin)
                          / alignment) * alignment;
        const qint64 headroom = qMax<qint64>(2LL * 1024 * 1024 * 1024, usedBytes / 10);
        if (sTarget <= 0 || usedBytes + headroom > sTarget) {
            fail(QString("Target disk %1 is too small. The system uses %2 GB (needs ~%3 GB "
                         "with headroom%5), but the target can hold only %4 GB for the system volume.")
                     .arg(targetIdx)
                     .arg(usedBytes / 1e9, 0, 'f', 1)
                     .arg((usedBytes + headroom) / 1e9, 0, 'f', 1)
                     .arg(qMax<qint64>(sTarget, 0) / 1e9, 0, 'f', 1)
                     .arg(keepRecovery ? QString(" plus %1 GB for WinRE").arg(recoveryTotal / 1e9, 0, 'f', 1)
                                       : QString()));
            return;
        }

        // Keep WinRE on the clone when (and only when) the source has it; the
        // mode-2 relocation path then packs it at the end of the target disk.
        relocateRecovery = keepRecovery;

        emit cloneStatus("clone_stage_shrink");
        // Ask Windows for the real minimum shrinkable size *before* attempting the
        // resize: our own usedBytes+headroom guess above is only about how much
        // data exists, not where on the volume it physically sits, so it can be
        // far too optimistic when a restore point, VSS storage, or a fragmented
        // file pins the volume's true floor much higher. Checking SizeMin first
        // turns a generic "Windows may be blocked by ..." guess (which the actual
        // PowerShell error text already made redundant) into an exact number the
        // user can act on, and skips a doomed Resize-Partition call entirely.
        auto runPs = [&](const QString &script, QString *outText) -> int {
            QProcess ps;
            ps.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                                 << "-Command" << script);
            ps.waitForFinished(600000);
            *outText = QString::fromLocal8Bit(
                ps.readAllStandardOutput() + "\n" + ps.readAllStandardError()).trimmed();
            return ps.exitCode();
        };
        auto queryVolume = [&](qint64 *origOut, qint64 *minOut) -> bool {
            QString q;
            runPs(QString("$ErrorActionPreference='Stop';"
                          "$p=Get-Partition -DriveLetter %1;"
                          "Write-Output ('ORIG='+$p.Size);"
                          "$sup=Get-PartitionSupportedSize -DriveLetter %1;"
                          "Write-Output ('MIN='+$sup.SizeMin)").arg(sysDrive.at(0)), &q);
            *origOut = *minOut = -1;
            for (const QString &line : q.split('\n')) {
                const QString t = line.trimmed();
                if (t.startsWith("ORIG=")) *origOut = t.mid(5).toLongLong();
                else if (t.startsWith("MIN=")) *minOut = t.mid(4).toLongLong();
            }
            return *origOut > 0 && *minOut > 0;
        };

        qint64 orig = -1, minSize = -1;
        if (!queryVolume(&orig, &minSize)) {
            fail("Could not read the system volume's size / shrinkable range from Windows. "
                 "The source disk is unchanged.");
            return;
        }

        bool markerWritten = false;
        QString reclaimed;
        if (orig > sTarget && minSize > sTarget) {
            // Windows' floor is above what the target can take. Release what pins
            // it (hibernation file, restore points) and ask again. The marker is
            // written first so an interrupted run is repaired on next launch.
            const bool hiberWasOn = hibernationEnabled();
            writeCloneMarker(sysDrive, orig, hiberWasOn);
            markerWritten = true;
            reclaimed = reclaimShrinkSpace(sysDrive, &hibernateDisabledByUs);
            const qint64 before = minSize;
            queryVolume(&orig, &minSize);
            CrashHandler::log(QString("Shrink clone: reclaimed space (%1) — shrink floor %2 GB -> %3 GB "
                                      "(target has room for %4 GB).")
                                  .arg(reclaimed).arg(before / 1e9, 0, 'f', 1)
                                  .arg(minSize / 1e9, 0, 'f', 1).arg(sTarget / 1e9, 0, 'f', 1));
        }

        if (orig > sTarget && minSize > sTarget) {
            fail(QString("The target disk is too small: the source system volume cannot be "
                         "shrunk below %1 GB, but the target only has room for %2 GB.\n"
                         "Already done automatically: %3\n"
                         "What still pins the volume is most likely the page file (it can only be "
                         "moved with a reboot) or a system file near the end of the volume. Try "
                         "setting the page file to another drive / \"No paging file\", reboot, and "
                         "clone again — or use a bigger target disk. The source disk is unchanged.")
                     .arg(minSize / 1e9, 0, 'f', 1).arg(sTarget / 1e9, 0, 'f', 1)
                     .arg(reclaimed.isEmpty() ? QString("nothing to release") : reclaimed));
            return;
        }

        bool shrunk = false;
        if (orig > sTarget) {
            if (!markerWritten)
                writeCloneMarker(sysDrive, orig, false);
            QString out;
            const int rc = runPs(QString("$ErrorActionPreference='Stop';"
                                         "Resize-Partition -DriveLetter %1 -Size %2;"
                                         "Write-Output 'SHRUNK'").arg(sysDrive.at(0)).arg(sTarget), &out);
            if (rc != 0 || !out.contains("SHRUNK")) {
                fail("Could not shrink the source system volume to fit the target. Windows may be "
                     "blocked by unmovable files (page file, hibernation, or a system file near the "
                     "end of the volume). The source disk is unchanged.\n" + out);
                return;
            }
            shrunk = true;
            srcRestoreSizeBytes = orig; // arm grow-back in cleanup()
        }
        CrashHandler::log(QString("Shrink clone: source %1 %2 (used %3 GB, target C: %4 GB).")
                              .arg(sysDrive)
                              .arg(shrunk ? "shrunk" : "already fit")
                              .arg(usedBytes / 1e9, 0, 'f', 1).arg(sTarget / 1e9, 0, 'f', 1));

        // Refresh the raw source handle's view of the now-smaller partition table.
        Sleep(1500);
        DeviceIoControl(hSource, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &io, NULL);
    }

    // ── source partition layout ──
    struct PartRange { qint64 offset; qint64 length; bool isSystemVolume; };
    QList<PartRange> parts;
    QList<QPair<qint64, qint64>> omittedRecovery;
    QList<RecoveryMove> recoveryMoves;
    bool sourceIsGpt = false;
    qint64 systemVolumeLength = 0;
    qint64 expandedSystemSize = 0;
    qint64 firstPartOffset = sourceSize;
    qint64 lastPartEnd = 0;
    {
        std::vector<BYTE> buf(65536, 0);
        if (!DeviceIoControl(hSource, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0,
                             buf.data(), DWORD(buf.size()), &io, NULL)) {
            fail("Failed to read the source partition layout.");
            return;
        }
        auto *layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(buf.data());
        sourceIsGpt = layout->PartitionStyle == PARTITION_STYLE_GPT;
        for (DWORD i = 0; i < layout->PartitionCount; ++i) {
            PARTITION_INFORMATION_EX &pe = layout->PartitionEntry[i];
            if (pe.PartitionLength.QuadPart <= 0)
                continue;
            static const GUID microsoftRecoveryGuid =
                {0xDE94BBA4, 0x06D1, 0x4D40, {0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC}};
            const bool isRecovery =
                (pe.PartitionStyle == PARTITION_STYLE_GPT &&
                 IsEqualGUID(pe.Gpt.PartitionType, microsoftRecoveryGuid)) ||
                (pe.PartitionStyle == PARTITION_STYLE_MBR && pe.Mbr.PartitionType == 0x27);
            if (!includeRecovery && isRecovery) {
                omittedRecovery.append(qMakePair(pe.StartingOffset.QuadPart,
                                                  pe.PartitionLength.QuadPart));
                RecoveryMove move;
                move.sourceOffset = pe.StartingOffset.QuadPart;
                move.length = pe.PartitionLength.QuadPart;
                recoveryMoves.append(move);
                continue;
            }
            PartRange r;
            r.offset = pe.StartingOffset.QuadPart;
            r.length = pe.PartitionLength.QuadPart;
            r.isSystemVolume = (r.offset == sysVolOffset);
            if (r.isSystemVolume)
                systemVolumeLength = r.length;
            parts.append(r);
            firstPartOffset = qMin(firstPartOffset, r.offset);
            lastPartEnd = qMax(lastPartEnd, r.offset + r.length);
        }
    }
    if (parts.isEmpty()) {
        fail("Source disk has no partitions.");
        return;
    }
    if (!includeRecovery && omittedRecovery.isEmpty())
        CrashHandler::log("System clone: no recovery partition was found on the source disk.");

    if (relocateRecovery) {
        if (!sourceIsGpt) {
            fail("Expanded system clone requires a GPT source disk.");
            return;
        }
        if (systemVolumeLength <= 0) {
            fail("The NTFS system partition could not be found in the source layout.");
            return;
        }
        // Auto-detect WinRE: carry a recovery partition across only when the
        // running system actually has WinRE enabled AND a recovery partition
        // exists to move. Otherwise the system volume simply grows to fill the
        // whole target disk (WinRE stored inside C:\Recovery travels with it).
        const bool winReActive = readWinReInfo(nullptr);
        const bool relocateWinRe = winReActive && !recoveryMoves.isEmpty();
        CrashHandler::log(QString("Expanded clone: WinRE %1, recovery partition %2 → %3")
                              .arg(winReActive ? "enabled" : "disabled")
                              .arg(recoveryMoves.isEmpty() ? "absent" : "present")
                              .arg(relocateWinRe ? "relocating WinRE" : "expanding system to fill disk"));
        const qint64 systemEnd = sysVolOffset + systemVolumeLength;
        for (const PartRange &part : parts) {
            if (!part.isSystemVolume && part.offset >= systemEnd) {
                fail("A non-recovery partition exists after the system partition; it cannot be safely expanded.");
                return;
            }
        }

        // Last usable GPT byte on the target (before the backup GPT).
        std::vector<BYTE> hdr(bps, 0);
        LARGE_INTEGER pos = {};
        pos.QuadPart = bps;
        if (!SetFilePointerEx(hSource, pos, nullptr, FILE_BEGIN) ||
            !ReadFile(hSource, hdr.data(), bps, &io, NULL) || io != bps ||
            memcmp(hdr.data(), "EFI PART", 8) != 0) {
            fail("Could not read the source GPT header.");
            return;
        }
        const quint32 entryCount = le32(hdr.data() + 0x50);
        const quint32 entrySize = le32(hdr.data() + 0x54);
        const qint64 entrySectors = (qint64(entryCount) * entrySize + bps - 1) / bps;
        const qint64 lastLba = targetSize / bps - 1;
        const qint64 lastUsableExclusive = (lastLba - entrySectors) * bps;
        const qint64 alignment = 1024LL * 1024;

        if (relocateWinRe) {
            for (const RecoveryMove &move : recoveryMoves) {
                if (move.sourceOffset < systemEnd) {
                    fail("Expanded system clone requires recovery partitions to be after the system partition.");
                    return;
                }
            }
            // Pack the source recovery partitions backwards from the last usable
            // byte on 1 MiB alignment; the system fills the space up to them.
            qint64 cursor = lastUsableExclusive;
            for (int i = recoveryMoves.size() - 1; i >= 0; --i) {
                cursor = ((cursor - recoveryMoves[i].length) / alignment) * alignment;
                recoveryMoves[i].targetOffset = cursor;
            }
            expandedSystemSize = recoveryMoves.first().targetOffset - sysVolOffset;
            lastPartEnd = recoveryMoves.constLast().targetOffset + recoveryMoves.constLast().length;
        } else {
            // No WinRE to carry — grow the system volume to the last usable byte.
            // Any source recovery partitions are dropped (their target entries
            // are already queued for removal via omittedRecovery).
            recoveryMoves.clear();
            const qint64 sysEndAligned = (lastUsableExclusive / alignment) * alignment;
            expandedSystemSize = sysEndAligned - sysVolOffset;
            lastPartEnd = sysVolOffset + expandedSystemSize;
        }
        if (expandedSystemSize < systemVolumeLength) {
            fail(QString("The target disk is too small to expand the system volume.\n"
                         "(mode %1, shrink-to-fit %2, source disk %3 GB, target disk %4 GB, "
                         "system volume %5 GB, room for it %6 GB)")
                     .arg(cloneMode).arg(shrinkToFit ? "on" : "off")
                     .arg(sourceSize / 1e9, 0, 'f', 1).arg(targetSize / 1e9, 0, 'f', 1)
                     .arg(systemVolumeLength / 1e9, 0, 'f', 1)
                     .arg(expandedSystemSize / 1e9, 0, 'f', 1));
            return;
        }
    }
    // Target must fit every partition plus the backup GPT (33 sectors is the
    // standard size; use the real entry count later in fixGptForTargetSize).
    if (!relocateRecovery && targetSize < lastPartEnd + qint64(34) * bps) {
        fail(QString("Destination is too small.\nRequired: %1 GB\nTarget: %2 GB")
                 .arg(lastPartEnd / (1024.0 * 1024 * 1024), 0, 'f', 1)
                 .arg(targetSize / (1024.0 * 1024 * 1024), 0, 'f', 1));
        return;
    }

    // ── VSS snapshot of the system volume for a consistent read ──
    // A consistent point-in-time snapshot is essential for a *system* clone:
    // copying the live, still-changing volume can capture the Windows Update
    // datastore (SoftwareDistribution\DataStore\DataStore.edb) or the registry
    // hives torn, which leaves the cloned Windows unable to install updates
    // ("Checking for updates" spins forever). So we make VSS work — enabling its
    // services and retrying — and refuse to silently copy the live volume.
    emit cloneStatus("clone_stage_snapshot");
    emit cloneProgressDetail(0, 0, 1, 0.0, 0);
    ensureVssServicesRunning();

    QString shadowDevice;
    for (int attempt = 0; attempt < 3 && hShadow == INVALID_HANDLE_VALUE; ++attempt) {
        if (attempt > 0) {
            CrashHandler::log(QString("System clone: retrying VSS snapshot (attempt %1 of 3)")
                                  .arg(attempt + 1));
            Sleep(1500);
        }
        QString sid;
        if (!createVssSnapshot(sysDrive, &shadowDevice, &sid)) {
            CrashHandler::log("System clone: VSS snapshot creation failed");
            continue;
        }
        HANDLE h = CreateFileW((LPCWSTR)shadowDevice.toStdWString().c_str(),
                               GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            CrashHandler::log("System clone: opening shadow device failed; deleting and retrying");
            deleteVssSnapshot(sid);
            continue;
        }
        hShadow = h;
        shadowId = sid;
        CrashHandler::log("System clone: reading system volume from VSS snapshot " + shadowDevice);
    }

    if (hShadow == INVALID_HANDLE_VALUE) {
        // Do NOT silently clone the live system volume: an inconsistent copy can
        // corrupt Windows Update / the registry on the target so it can never
        // update. Stop with actionable guidance instead.
        fail("Could not create a Volume Shadow Copy (VSS) snapshot of the system "
             "volume after several attempts.\n\nCloning the live system volume "
             "would risk an inconsistent copy that cannot install Windows updates, "
             "so the clone was stopped.\n\nMake sure the 'Volume Shadow Copy' (VSS) "
             "and 'Microsoft Software Shadow Copy Provider' (swprv) services are "
             "not disabled, and that the system drive has enough free space for a "
             "shadow copy, then try again.");
        return;
    }

    // ── NTFS geometry of the system volume ──
    NTFS_VOLUME_DATA_BUFFER nvd = {};
    if (!DeviceIoControl(hShadow, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &nvd, sizeof(nvd), &io, NULL)) {
        fail("The system volume does not report NTFS data (unsupported filesystem?).");
        return;
    }
    const qint64 clusterSize = nvd.BytesPerCluster;
    const qint64 totalClusters = nvd.TotalClusters.QuadPart;
    const qint64 allocatedBytes = (totalClusters - nvd.FreeClusters.QuadPart) * clusterSize;

    // ── plan totals for progress ──
    qint64 totalBytes = firstPartOffset; // MBR/GPT + alignment gap
    for (const auto &p : parts)
        totalBytes += p.isSystemVolume ? allocatedBytes : p.length;
    if (relocateRecovery) {
        for (const RecoveryMove &move : recoveryMoves)
            totalBytes += move.length;
    }

    const qint64 bufferSize = 64LL * 1024 * 1024;
    // Page-aligned buffer: FILE_FLAG_NO_BUFFERING writes require the buffer,
    // the file offset and the byte count to all be sector-aligned. Every copy
    // offset/length here is already a multiple of the sector size.
    alignedBuf = VirtualAlloc(NULL, bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!alignedBuf) { fail("Could not allocate the copy buffer."); return; }
    char *buffer = static_cast<char *>(alignedBuf);

    // Opens the bulk-copy write handle. The first call probes whether the
    // device accepts unbuffered (direct) writes with a throwaway aligned write
    // at offset 0 — which the real MBR/GPT copy overwrites moments later — and
    // remembers the result so later reopens (expanded mode) skip the probe and
    // never clobber already-written data. Falls back to a buffered handle when
    // unbuffered I/O is not available, so speed only ever improves.
    const std::wstring targetPathW = targetPath.toStdWString();
    const DWORD shareRW = FILE_SHARE_READ | FILE_SHARE_WRITE;
    bool fastProbed = false, fastOk = false;
    auto openFastTarget = [&]() -> HANDLE {
        if (!fastProbed) {
            fastProbed = true;
            // Right after clearing the drive the storage stack may still be
            // re-scanning the target, so the first unbuffered write can transiently
            // fail. Retry a few times before giving up — unbuffered writes are
            // ~3x faster to USB bridges, so it's worth insisting.
            for (int attempt = 0; attempt < 8; ++attempt) {
                HANDLE h = CreateFileW(targetPathW.c_str(), GENERIC_READ | GENERIC_WRITE,
                                       shareRW, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    memset(alignedBuf, 0, bps);
                    LARGE_INTEGER z; z.QuadPart = 0; DWORD w = 0;
                    if (SetFilePointerEx(h, z, nullptr, FILE_BEGIN) &&
                        WriteFile(h, alignedBuf, bps, &w, NULL) && w == bps) {
                        fastOk = true;
                        CrashHandler::log(QString("System clone: using unbuffered (direct) target "
                                                  "writes (attempt %1).").arg(attempt + 1));
                        return h;
                    }
                    CrashHandler::log(QString("System clone: unbuffered probe attempt %1 failed "
                                              "(error %2), retrying...").arg(attempt + 1).arg(GetLastError()));
                    CloseHandle(h);
                }
                Sleep(400);
            }
            CrashHandler::log("System clone: unbuffered writes unavailable after retries; using buffered writes.");
        } else if (fastOk) {
            HANDLE h = CreateFileW(targetPathW.c_str(), GENERIC_READ | GENERIC_WRITE,
                                   shareRW, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
            if (h != INVALID_HANDLE_VALUE)
                return h;
        }
        return CreateFileW(targetPathW.c_str(), GENERIC_READ | GENERIC_WRITE,
                           shareRW, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    };
    hTargetCopy = openFastTarget();
    if (hTargetCopy == INVALID_HANDLE_VALUE) { fail("Could not open the target disk for writing."); return; }

    qint64 copiedBytes = 0;
    int lastPercent = -1;
    auto t0 = std::chrono::steady_clock::now();
    auto lastLog = t0;
    // Instantaneous (recent-window) speed, so a mid-clone slowdown — e.g. the
    // target SSD's SLC write cache filling — shows immediately instead of being
    // hidden by a cumulative average that only drifts slowly.
    qint64 winBytes = 0;
    auto winTime = t0;
    auto lastEmit = t0;
    double instMibPerSec = 0.0;

    auto reportProgress = [&]() {
        int percent = int((copiedBytes * 100) / qMax<qint64>(totalBytes, 1));
        auto now = std::chrono::steady_clock::now();
        const double totalSec = std::chrono::duration_cast<std::chrono::duration<double>>(now - t0).count();
        const double winSec = std::chrono::duration_cast<std::chrono::duration<double>>(now - winTime).count();
        if (winSec >= 0.75) {
            instMibPerSec = double(copiedBytes - winBytes) / winSec / (1024.0 * 1024.0);
            winBytes = copiedBytes;
            winTime = now;
        }
        // Recent-window rate once available; cumulative for the first fraction of a second.
        double mibPerSec = instMibPerSec > 0.0
            ? instMibPerSec
            : (totalSec > 0.0 ? double(copiedBytes) / totalSec / (1024.0 * 1024.0) : 0.0);
        const double bytesPerSec = mibPerSec * 1024.0 * 1024.0;
        qint64 eta = (bytesPerSec > 0.0) ? qint64((totalBytes - copiedBytes) / bytesPerSec) : 0;
        const bool pctChanged = (percent != lastPercent);
        auto msSinceEmit = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEmit).count();
        if (pctChanged || msSinceEmit >= 500) {
            lastEmit = now;
            if (pctChanged) {
                lastPercent = percent;
                emit cloneProgress(percent);
            }
            emit cloneProgressDetail(percent, copiedBytes, totalBytes, mibPerSec, eta);
        }
        auto msSinceLog = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog).count();
        if (msSinceLog >= 5000) {
            CrashHandler::log(QString("System clone %1% (%2/%3 MiB, %4 MiB/s now, %5 MiB/s avg)")
                                  .arg(percent)
                                  .arg(copiedBytes / (1024 * 1024))
                                  .arg(totalBytes / (1024 * 1024))
                                  .arg(mibPerSec, 0, 'f', 1)
                                  .arg(totalSec > 0.0 ? double(copiedBytes) / totalSec / (1024.0 * 1024.0) : 0.0, 0, 'f', 1));
            lastLog = now;
        }
    };

    // Resilient write to the target. USB-NVMe/SATA bridges sometimes fail big
    // transfers with ERROR_IO_DEVICE (1117) even though small ones work, so on a
    // failure we retry, then permanently drop to smaller transfer sizes, then
    // fall back to a buffered handle. Degradation is sticky for the rest of the
    // clone so it only costs time once.
    qint64 maxWrite = bufferSize;
    bool bufferedFallback = false;
    auto writeTarget = [&](qint64 pos, const char *src, DWORD len, QString *werr) -> bool {
        DWORD done = 0;
        int failures = 0;
        DWORD lastGle = 0;
        while (done < len) {
            const DWORD piece = DWORD(qMin<qint64>(maxWrite, len - done));
            LARGE_INTEGER lp; lp.QuadPart = pos + done;
            DWORD w = 0;
            if (SetFilePointerEx(hTargetCopy, lp, nullptr, FILE_BEGIN) &&
                WriteFile(hTargetCopy, src + done, piece, &w, NULL) && w > 0) {
                done += w;
                continue;
            }
            lastGle = GetLastError();
            ++failures;
            CrashHandler::log(QString("System clone: write of %1 B at offset %2 failed (error %3, "
                                      "failure #%4, max write %5 B, %6).")
                                  .arg(piece).arg(pos + done).arg(lastGle).arg(failures)
                                  .arg(maxWrite).arg(bufferedFallback ? "buffered" : "direct"));
            if (m_cloneCancelRequested.load()) { *werr = "__CANCELLED__"; return false; }
            if (failures == 1) {
                Sleep(500);                       // transient: just retry once
            } else if (maxWrite > 64 * 1024) {
                maxWrite = qMax<qint64>(64 * 1024, maxWrite / 8); // smaller transfers
                Sleep(300);
            } else if (!bufferedFallback) {
                // Last resort: reopen without NO_BUFFERING (also resets the handle).
                bufferedFallback = true;
                maxWrite = 1024 * 1024;
                CloseHandle(hTargetCopy);
                hTargetCopy = CreateFileW(targetPathW.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          shareRW, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (hTargetCopy == INVALID_HANDLE_VALUE) {
                    *werr = QString("Could not reopen the target disk (error %1).").arg(GetLastError());
                    return false;
                }
                Sleep(500);
            } else if (failures < 14) {
                Sleep(1000);                      // buffered, 64 KiB-1 MiB: keep trying a while
            } else {
                *werr = QString("Write failed at offset %1 (error %2).").arg(pos + done).arg(lastGle);
                return false;
            }
        }
        return true;
    };

    // Raw-copy a byte range between the same offsets on source and target disks.
    auto rawCopy = [&](HANDLE hFrom, qint64 fromBase, qint64 toBase, qint64 length, QString *err) -> bool {
        qint64 done = 0;
        while (done < length) {
            if (m_cloneCancelRequested.load()) {
                *err = "__CANCELLED__";
                return false;
            }
            const DWORD chunk = DWORD(qMin<qint64>(bufferSize, length - done));
            LARGE_INTEGER pos;
            DWORD r = 0, w = 0;
            pos.QuadPart = fromBase + done;
            if (!SetFilePointerEx(hFrom, pos, nullptr, FILE_BEGIN) ||
                !ReadFile(hFrom, buffer, chunk, &r, NULL) || r == 0) {
                *err = QString("Read failed at offset %1 (error %2).").arg(fromBase + done).arg(GetLastError());
                return false;
            }
            if (!writeTarget(toBase + done, buffer, r, err))
                return false;
            done += r;
            copiedBytes += r;
            reportProgress();
        }
        return true;
    };

    QString err;

    // 1. Pre-partition area: protective MBR, primary GPT, alignment gap.
    emit cloneStatus("clone_stage_copy");
    if (!rawCopy(hSource, 0, 0, firstPartOffset, &err)) { fail(err); return; }

    // 2. Partitions.
    for (const auto &p : parts) {
        if (!p.isSystemVolume) {
            // EFI / MSR / WinRE / other partitions: raw copy (small on system disks).
            if (!rawCopy(hSource, p.offset, p.offset, p.length, &err)) { fail(err); return; }
            continue;
        }

        // System volume: copy only allocated NTFS clusters, reading from the snapshot.
        std::vector<BYTE> bmBuf(sizeof(VOLUME_BITMAP_BUFFER) + 4 * 1024 * 1024);
        qint64 lcn = 0;
        qint64 runStart = -1;
        qint64 runEnd = -1;
        // Merge runs separated by small gaps: one big sequential read beats many
        // small seeks, and copying a few free clusters is harmless.
        const qint64 gapTolerance = qMax<qint64>(1, (2LL * 1024 * 1024) / clusterSize);

        auto flushRun = [&]() -> bool {
            if (runStart < 0)
                return true;
            qint64 byteOff = runStart * clusterSize;
            qint64 byteLen = (runEnd - runStart) * clusterSize;
            // Clip to volume length (last cluster run may reach the boot-sector copy area).
            byteLen = qMin(byteLen, p.length - byteOff);
            qint64 done = 0;
            while (done < byteLen) {
                if (m_cloneCancelRequested.load()) {
                    err = "__CANCELLED__";
                    return false;
                }
                const DWORD chunk = DWORD(qMin<qint64>(bufferSize, byteLen - done));
                LARGE_INTEGER pos;
                DWORD r = 0, w = 0;
                pos.QuadPart = byteOff + done;
                if (!SetFilePointerEx(hShadow, pos, nullptr, FILE_BEGIN) ||
                    !ReadFile(hShadow, buffer, chunk, &r, NULL) || r == 0) {
                    err = QString("Volume read failed at offset %1 (error %2).").arg(byteOff + done).arg(GetLastError());
                    return false;
                }
                if (!writeTarget(p.offset + byteOff + done, buffer, r, &err))
                    return false;
                done += r;
                copiedBytes += r;
                reportProgress();
            }
            runStart = runEnd = -1;
            return true;
        };

        bool bitmapDone = false;
        while (!bitmapDone) {
            STARTING_LCN_INPUT_BUFFER inb;
            inb.StartingLcn.QuadPart = lcn;
            DWORD ret = 0;
            auto *vbb = reinterpret_cast<VOLUME_BITMAP_BUFFER*>(bmBuf.data());
            BOOL ok = DeviceIoControl(hShadow, FSCTL_GET_VOLUME_BITMAP, &inb, sizeof(inb),
                                      vbb, DWORD(bmBuf.size()), &ret, NULL);
            const DWORD gle = GetLastError();
            if (!ok && gle != ERROR_MORE_DATA) {
                fail(QString("FSCTL_GET_VOLUME_BITMAP failed (error %1).").arg(gle));
                return;
            }
            bitmapDone = (ok != FALSE);

            const qint64 chunkStart = vbb->StartingLcn.QuadPart;
            const qint64 availBytes = qint64(ret) - qint64(offsetof(VOLUME_BITMAP_BUFFER, Buffer));
            const qint64 bitsAvail = qMin<qint64>(vbb->BitmapSize.QuadPart, availBytes * 8);

            qint64 i = lcn - chunkStart; // skip bits we already processed
            while (i < bitsAvail) {
                const BYTE b = vbb->Buffer[i >> 3];
                if ((i & 7) == 0 && (b == 0x00 || b == 0xFF) && i + 8 <= bitsAvail) {
                    if (b == 0xFF) {
                        const qint64 c = chunkStart + i;
                        if (runStart < 0) { runStart = c; runEnd = c + 8; }
                        else if (c - runEnd <= gapTolerance &&
                                 (c + 8 - runStart) * clusterSize < bufferSize * 4) { runEnd = c + 8; }
                        else { if (!flushRun()) { fail(err); return; } runStart = c; runEnd = c + 8; }
                    } else if (runStart >= 0 && (chunkStart + i) - runEnd > gapTolerance) {
                        if (!flushRun()) { fail(err); return; }
                    }
                    i += 8;
                    continue;
                }
                if (b & (1 << (i & 7))) {
                    const qint64 c = chunkStart + i;
                    if (runStart < 0) { runStart = c; runEnd = c + 1; }
                    else if (c - runEnd <= gapTolerance &&
                             (c + 1 - runStart) * clusterSize < bufferSize * 4) { runEnd = c + 1; }
                    else { if (!flushRun()) { fail(err); return; } runStart = c; runEnd = c + 1; }
                }
                ++i;
            }
            lcn = chunkStart + bitsAvail;
            if (bitsAvail <= 0)
                break;
        }
        if (!flushRun()) { fail(err); return; }

        // NTFS keeps a backup boot sector in the last sector of the partition,
        // outside the cluster area — copy it from the source disk directly.
        if (!rawCopy(hSource, p.offset + p.length - bps, p.offset + p.length - bps, bps, &err)) {
            fail(err);
            return;
        }
    }

    // Flush and release the unbuffered copy handle so the buffered GPT edits
    // below read back the partition tables the bulk copy just wrote.
    FlushFileBuffers(hTargetCopy);
    CloseHandle(hTargetCopy);
    hTargetCopy = INVALID_HANDLE_VALUE;

    // 3. Remove excluded recovery entries, then fix the backup GPT.
    if (!removePartitionsFromTargetTable(hTarget, bps, omittedRecovery, &err)) {
        fail(err);
        return;
    }
    if (!fixGptForTargetSize(hTarget, targetSize, bps, lastPartEnd / bps, &err)) {
        fail(err);
        return;
    }

    if (relocateRecovery) {
        if (!setTemporaryGptDiskGuid(hTarget, targetSize, bps, lastPartEnd / bps, &err)) {
            fail(err);
            return;
        }
        if (!FlushFileBuffers(hTarget)) {
            fail(QString("Flush before system-volume expansion failed (error %1).").arg(GetLastError()));
            return;
        }
        DeviceIoControl(hTarget, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &io, NULL);

        // Close raw handles while the Windows storage stack mounts the target and
        // Resize-Partition grows both the GPT entry and the NTFS filesystem. The
        // target is taken offline again before its original GPT identity returns.
        if (hShadow != INVALID_HANDLE_VALUE) { CloseHandle(hShadow); hShadow = INVALID_HANDLE_VALUE; }
        if (hSource != INVALID_HANDLE_VALUE) { CloseHandle(hSource); hSource = INVALID_HANDLE_VALUE; }
        if (hTarget != INVALID_HANDLE_VALUE) { CloseHandle(hTarget); hTarget = INVALID_HANDLE_VALUE; }
        deleteVssSnapshot(shadowId);
        shadowId.clear();

        emit cloneStatus("clone_stage_expand");
        // Slow (typically USB) targets can take many seconds to enumerate, so
        // poll for the partition instead of a fixed sleep.
        const QString resizeScript = QString(
            "$ErrorActionPreference='Stop'; "
            "try { "
            "  Set-Disk -Number %1 -IsReadOnly $false -ErrorAction SilentlyContinue; "
            "  Set-Disk -Number %1 -IsOffline $false; Update-Disk -Number %1; "
            "  $p = $null; "
            "  for ($i = 0; $i -lt 60 -and -not $p; $i++) { "
            "    Start-Sleep -Milliseconds 500; "
            "    $p = Get-Partition -DiskNumber %1 -ErrorAction SilentlyContinue | "
            "         Where-Object { [UInt64]$_.Offset -eq [UInt64]%2 } | Select-Object -First 1; "
            "    if (-not $p -and ($i -eq 20 -or $i -eq 40)) { Update-Disk -Number %1 -ErrorAction SilentlyContinue }; "
            "  }; "
            "  if (-not $p) { throw 'Target system partition was not mounted.' }; "
            "  Resize-Partition -InputObject $p -Size ([UInt64]%3); "
            "  Write-Output ('RESIZED|' + $p.PartitionNumber); "
            "} finally { Set-Disk -Number %1 -IsOffline $true -ErrorAction SilentlyContinue }"
        ).arg(targetIdx).arg(sysVolOffset).arg(expandedSystemSize);
        QProcess resizeProcess;
        resizeProcess.start("powershell", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass"
                                                       << "-Command" << resizeScript);
        const bool resizeTimedOut = !resizeProcess.waitForFinished(300000);
        QString resizeOutput;
        if (resizeTimedOut) {
            resizeProcess.kill();
            resizeProcess.waitForFinished(5000);
            // The killed script never reached its finally block; force the
            // target offline so the raw GPT salvage below is safe.
            QProcess::execute("powershell", QStringList() << "-NoProfile" << "-Command"
                << QString("Set-Disk -Number %1 -IsOffline $true -ErrorAction SilentlyContinue").arg(targetIdx));
        } else {
            resizeOutput = QString::fromLocal8Bit(
                resizeProcess.readAllStandardOutput() + "\n" + resizeProcess.readAllStandardError()).trimmed();
            if (!resizeOutput.isEmpty())
                CrashHandler::log("Expanded clone Resize-Partition output: " + resizeOutput);
        }
        const bool resizeOk = !resizeTimedOut &&
            resizeProcess.exitStatus() == QProcess::NormalExit && resizeProcess.exitCode() == 0 &&
            resizeOutput.contains("RESIZED|");

        hSource = CreateFileW((LPCWSTR)sourcePath.toStdWString().c_str(),
                              GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        hTarget = CreateFileW((LPCWSTR)targetPath.toStdWString().c_str(),
                              GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hSource == INVALID_HANDLE_VALUE || hTarget == INVALID_HANDLE_VALUE) {
            fail("Could not reopen the disks to restore WinRE after expanding the system volume.");
            return;
        }
        DeviceIoControl(hTarget, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &io, NULL);

        // Once the temporary identity has been written, a plain fail() would
        // leave the target with throwaway GUIDs that the cloned BCD does not
        // reference. Put the original identity back (best effort) so the
        // target still boots — just without WinRE. Cancellation keeps its
        // exact "__CANCELLED__" message, which the UI matches verbatim.
        auto failKeepingBootable = [&](const QString &msg) {
            QString restoreErr;
            const bool restored = restoreRelocatedRecoveryGpt(
                hSource, hTarget, targetSize, bps, sysVolOffset,
                QList<RecoveryMove>(), &restoreErr);
            CrashHandler::log(restored
                ? QStringLiteral("Expanded clone: original GPT identity restored after a failure; target remains bootable without WinRE.")
                : "Expanded clone: could not restore the original GPT identity: " + restoreErr);
            if (!restored || msg == "__CANCELLED__")
                fail(msg);
            else
                fail(msg + " The target keeps a bootable clone without WinRE.");
        };

        if (!resizeOk) {
            failKeepingBootable(resizeTimedOut
                ? QStringLiteral("Timed out while expanding the target system volume.")
                : "Windows could not expand the cloned NTFS system volume. " + resizeOutput);
            return;
        }

        // Copy WinRE to its new end-of-disk home (skipped when there is no
        // recovery partition to relocate — the system volume already fills it).
        if (!recoveryMoves.isEmpty()) {
            emit cloneStatus("clone_stage_winre");
            hTargetCopy = openFastTarget();
            if (hTargetCopy == INVALID_HANDLE_VALUE) {
                failKeepingBootable("Could not reopen the target disk to copy WinRE.");
                return;
            }
            for (const RecoveryMove &move : recoveryMoves) {
                if (!rawCopy(hSource, move.sourceOffset, move.targetOffset, move.length, &err)) {
                    failKeepingBootable(err);
                    return;
                }
            }
            // Flush and release the unbuffered handle before the buffered GPT
            // restore reads and rewrites the relocated recovery entries.
            FlushFileBuffers(hTargetCopy);
            CloseHandle(hTargetCopy);
            hTargetCopy = INVALID_HANDLE_VALUE;
        }
        // Restore original GPT identity (and, when present, the relocated
        // recovery entries). With an empty move list this only puts the
        // original disk/partition GUIDs back and fixes the backup GPT.
        if (!restoreRelocatedRecoveryGpt(hSource, hTarget, targetSize, bps,
                                         sysVolOffset, recoveryMoves, &err)) {
            failKeepingBootable(err);
            return;
        }
        if (recoveryMoves.isEmpty())
            CrashHandler::log(QString("Expanded clone: system volume expanded to %1 GB (no WinRE to relocate).")
                                  .arg(expandedSystemSize / (1024.0 * 1024 * 1024), 0, 'f', 1));
        else
            CrashHandler::log(QString("Expanded clone: system partition resized to %1 GB; WinRE moved to offset %2")
                                  .arg(expandedSystemSize / (1024.0 * 1024 * 1024), 0, 'f', 1)
                                  .arg(recoveryMoves.first().targetOffset));
    }

    if (!FlushFileBuffers(hTarget)) {
        fail(QString("Flush failed (error %1).").arg(GetLastError()));
        return;
    }
    DeviceIoControl(hTarget, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &io, NULL);

    auto end = std::chrono::steady_clock::now();
    const double totalSec = std::chrono::duration_cast<std::chrono::duration<double>>(end - t0).count();
    const double avgMiBps = totalSec > 0 ? copiedBytes / (1024.0 * 1024.0) / totalSec : 0.0;
    CrashHandler::log(QString("System clone finished: %1 MiB in %2 s (%3 MiB/s)")
                          .arg(copiedBytes / (1024 * 1024)).arg(totalSec, 0, 'f', 1).arg(avgMiBps, 0, 'f', 1));

    cleanup();

    emit cloneFinished(true, QString("Copied %1 GB in %2 min (%3 MiB/s average).")
                                 .arg(copiedBytes / (1024.0 * 1024 * 1024), 0, 'f', 1)
                                 .arg(totalSec / 60.0, 0, 'f', 1)
                                 .arg(avgMiBps, 0, 'f', 0));
    #else
    Q_UNUSED(sourceIdx);
    Q_UNUSED(targetIdx);
    Q_UNUSED(cloneMode);
    emit cloneFinished(false, "Not supported on this OS.");
    #endif
}

// Maps an SMBIOS memory-type code (Win32_PhysicalMemory.SMBIOSMemoryType) to a
// name; unknown codes give an empty string.
static QString memTypeFromSmbios(int t) {
    switch (t) {
        case 20: return "DDR";
        case 21: return "DDR2";
        case 24: return "DDR3";
        case 26: return "DDR4";
        case 34: return "DDR5";
        default: return "";
    }
}

// Collects the system summary (model, OS, activation, CPU, GPU/NPU, RAM, network,
// encryption) into `overview`, emitting cardReady() and progress() as each stage
// completes, and returns a plain-text report.
QString HardwareModel::getWindowsInfo(std::function<void(int, int, QString)> progressCallback, SystemSummaryData &overview) {
    QStringList lines;
    lines << "=== Windows Hardware Information ===" << "";

    int step = 1;
    int total = 12;

    QElapsedTimer stepTimer;
    stepTimer.start();

    // 0. System model (Win32_ComputerSystem)
    auto cs = queryWmi("SELECT Manufacturer, Model, SystemFamily FROM Win32_ComputerSystem");
    if (!cs.isEmpty()) {
        QString mfr   = cs.first()["Manufacturer"].toString().trimmed();
        QString model = cs.first()["Model"].toString().trimmed();
        // Lenovo stores the marketed name in SMBIOS product Version, while
        // Model often contains only the machine type (e.g. 20W4...).
        if (mfr.contains("Lenovo", Qt::CaseInsensitive)) {
            auto products = queryWmi("SELECT Version FROM Win32_ComputerSystemProduct");
            QStringList candidates;
            if (!products.isEmpty())
                candidates << products.first()["Version"].toString().trimmed();
            candidates << cs.first()["SystemFamily"].toString().trimmed();
            for (const QString &candidate : candidates) {
                // Require a specific model, not a generic family or BIOS placeholder.
                if (candidate.compare(model, Qt::CaseInsensitive) == 0 ||
                    candidate.contains("to be filled", Qt::CaseInsensitive) ||
                    !candidate.contains(QRegularExpression("[A-Za-z]")) ||
                    !candidate.contains(QRegularExpression("[0-9]")) ||
                    !candidate.contains(' '))
                    continue;
                model = candidate;
                break;
            }
        }
        if (!mfr.isEmpty() && !model.startsWith(mfr, Qt::CaseInsensitive))
            overview.systemModel = mfr + " " + model;
        else
            overview.systemModel = model;
    }
    CrashHandler::log(QString("Read system model in %1 ms").arg(stepTimer.restart()));

    // 1. CPU
    progressCallback(step++, total, "Reading CPU information");
    lines << "=== CPU (WMI: Win32_Processor) ===";
    auto cpus = queryWmi("SELECT Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed FROM Win32_Processor");
    for (const auto &cpu : cpus) {
        lines << "Name          : " + cpu["Name"].toString();
        lines << "Manufacturer  : " + cpu["Manufacturer"].toString();
        lines << "Cores         : " + cpu["NumberOfCores"].toString();
        lines << "Logical CPUs  : " + cpu["NumberOfLogicalProcessors"].toString();
        lines << "Max Clock     : " + cpu["MaxClockSpeed"].toString() + " MHz";
        overview.cpuName    = cpu["Name"].toString().trimmed();
        overview.cpuCores   = cpu["NumberOfCores"].toInt();
        overview.cpuThreads = cpu["NumberOfLogicalProcessors"].toInt();
        break;
    }
    // NPU / AI accelerator: Windows 11 lists these under the ComputeAccelerator
    // PnP class; fall back to well-known names for older drivers. A bare '%NPU%'
    // would also match "Input", so the name pattern needs a leading space.
    {
        auto npuDevs = queryWmi("SELECT Name, Status FROM Win32_PnPEntity WHERE Present=TRUE AND "
            "(PNPClass='ComputeAccelerator' OR Name LIKE '% NPU%' OR Name LIKE 'NPU%' "
            "OR Name LIKE '%AI Boost%' OR Name LIKE '%Ryzen AI%' OR Name LIKE '%Hexagon%')");
        for (const auto &d : npuDevs) {
            const QString n = d["Name"].toString().trimmed();
            if (n.isEmpty() || overview.npus.contains(n)) continue;
            overview.npus.append(n);
            lines << "NPU           : " + n;
        }
    }
    lines << "";
    emit cardReady("cpu", overview);
    CrashHandler::log(QString("Read CPU information in %1 ms").arg(stepTimer.restart()));

    // 2. GPU (DXGI for accurate VRAM)
    progressCallback(step++, total, "Reading GPU information");
    lines << "=== GPU (DXGI) ===";
    #ifdef Q_OS_WIN
    IDXGIFactory* pFactory = NULL;
    if (CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory) == S_OK) {
        IDXGIAdapter* pAdapter = NULL;
        QSet<QString> seenGpus;
        for (UINT i = 0; pFactory->EnumAdapters(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC desc;
            pAdapter->GetDesc(&desc);

            QString name = QString::fromWCharArray(desc.Description);

            // Skip duplicates (e.g. same GPU listed multiple times or Microsoft Basic Render Driver if desired)
            if (seenGpus.contains(name)) {
                pAdapter->Release();
                continue;
            }
            seenGpus.insert(name);

            qint64 vram = desc.DedicatedVideoMemory;

            lines << "Name          : " + name;
            lines << "Dedicated VRAM: " + QString::number(vram / (1024*1024)) + " MB";
            lines << "----------------------------------------";

            GpuEntry g;
            g.name   = name;
            g.vramMb = vram / (1024 * 1024);
            overview.gpus.append(g);

            pAdapter->Release();
        }
        pFactory->Release();
    } else {
        lines << "Failed to initialize DXGI.";
    }
    #endif
    lines << "";
    emit cardReady("gpu", overview);
    CrashHandler::log(QString("Read GPU information in %1 ms").arg(stepTimer.restart()));

    // 3. RAM
    progressCallback(step++, total, "Reading memory information");
    lines << "=== Memory (RAM) ===";
    // Using GlobalMemoryStatusEx for RAM as it's easier than WMI for totals
    #ifdef Q_OS_WIN
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    double totalPhys = memInfo.ullTotalPhys / (1024.0 * 1024 * 1024);
    double availPhys = memInfo.ullAvailPhys / (1024.0 * 1024 * 1024);
    double usedPhys = totalPhys - availPhys;
    double percent = memInfo.dwMemoryLoad;

    lines << QString("Total       : %1 GB").arg(totalPhys, 0, 'f', 2);
    lines << QString("Available   : %1 GB").arg(availPhys, 0, 'f', 2);
    lines << QString("Used        : %1 GB").arg(usedPhys, 0, 'f', 2);
    lines << QString("Percent     : %1%").arg(percent);

    overview.totalRamBytes = memInfo.ullTotalPhys;

    // Total number of physical memory slots on the board.
    auto memArray = queryWmi("SELECT MemoryDevices FROM Win32_PhysicalMemoryArray");
    for (const auto &a : memArray) {
        int slotCount = a["MemoryDevices"].toInt();
        if (slotCount > 0) { overview.ramSlotsTotal = slotCount; break; }
    }

    // Get RAM sticks, speed, type, manufacturer, and slot via WMI.
    auto mems = queryWmi("SELECT Speed, ConfiguredClockSpeed, Capacity, SMBIOSMemoryType, Manufacturer, DeviceLocator, BankLabel FROM Win32_PhysicalMemory");
    if (!mems.isEmpty()) {
        int speed = 0;
        int memTypeCode = 0;
        for (const auto &m : mems) {
            qint64 cap = m["Capacity"].toLongLong();
            if (cap > 0) overview.ramStickSizes.append(cap);

            int stickSpeed = 0;
            if (m["ConfiguredClockSpeed"].isValid() && m["ConfiguredClockSpeed"].toInt() > 0)
                stickSpeed = m["ConfiguredClockSpeed"].toInt();
            else if (m["Speed"].isValid() && m["Speed"].toInt() > 0)
                stickSpeed = m["Speed"].toInt();

            int stickTypeCode = m["SMBIOSMemoryType"].isValid() ? m["SMBIOSMemoryType"].toInt() : 0;

            if (speed == 0 && stickSpeed > 0)       speed = stickSpeed;
            if (memTypeCode == 0 && stickTypeCode)  memTypeCode = stickTypeCode;

            RamStickEntry rs;
            rs.sizeBytes = cap;
            rs.speedMhz = stickSpeed;
            rs.type = memTypeFromSmbios(stickTypeCode);
            rs.manufacturer = m["Manufacturer"].toString().trimmed();
            rs.slot = m["DeviceLocator"].toString().trimmed();
            if (rs.slot.isEmpty()) rs.slot = m["BankLabel"].toString().trimmed();
            overview.ramSticks.append(rs);

            lines << QString("Slot        : %1  |  %2 GB  |  %3  |  %4 MHz  |  %5")
                        .arg(rs.slot.isEmpty() ? "?" : rs.slot)
                        .arg(cap / (1024.0 * 1024 * 1024), 0, 'f', 0)
                        .arg(rs.type.isEmpty() ? "?" : rs.type)
                        .arg(stickSpeed)
                        .arg(rs.manufacturer.isEmpty() ? "?" : rs.manufacturer);
        }
        overview.ramSpeedMhz = speed;
        overview.ramType = memTypeFromSmbios(memTypeCode);
        lines << QString("Slots       : %1 used / %2 total")
                    .arg(overview.ramSticks.size())
                    .arg(overview.ramSlotsTotal > 0 ? QString::number(overview.ramSlotsTotal) : "?");
        if (speed > 0)
            lines << "Frequency   : " + QString::number(speed) + " MHz";
        else
            lines << "Frequency   : Unknown";
    }
    #endif
    lines << "";
    emit cardReady("memory", overview);
    CrashHandler::log(QString("Read RAM information in %1 ms").arg(stepTimer.restart()));

    // 4. OS
    progressCallback(step++, total, "Reading OS details");
    lines << "=== OS (WMI: Win32_OperatingSystem) ===";
    auto osList = queryWmi("SELECT Caption, Version, BuildNumber, OSArchitecture FROM Win32_OperatingSystem");
    for (const auto &os : osList) {
        lines << "Name          : " + os["Caption"].toString();
        lines << "Version       : " + os["Version"].toString();
        lines << "Build Number  : " + os["BuildNumber"].toString();
        lines << "Architecture  : " + os["OSArchitecture"].toString();
        if (overview.osName.isEmpty())
            overview.osName = os["Caption"].toString().trimmed();
    }
    lines << "";
    emit cardReady("system", overview);
    CrashHandler::log(QString("Read OS details in %1 ms").arg(stepTimer.restart()));

    // 5. Activation
    progressCallback(step++, total, "Checking Windows activation status");
    lines << "=== Windows Activation ===";
    // Windows OS licenses are identified by the well-known Windows ApplicationID.
    // Filtering by it (instead of matching the word "Windows" in a product name)
    // avoids false positives from unrelated licensing products. A product is only
    // genuinely activated when LicenseStatus == 1 (Licensed).
    const QString WIN_APP_ID = "55c92734-d682-4d71-983e-d6ec3f16059f";
    auto winProducts = queryWmi(
        "SELECT Name, LicenseStatus, PartialProductKey, ApplicationID "
        "FROM SoftwareLicensingProduct "
        "WHERE ApplicationID='" + WIN_APP_ID + "' AND PartialProductKey IS NOT NULL");

    auto licenseStatusText = [](int status) -> QString {
        switch (status) {
            case 0: return "Unlicensed";
            case 1: return "Licensed (Activated)";
            case 2: return "Out-of-Box Grace";
            case 3: return "Out-of-Tolerance Grace";
            case 4: return "Non-Genuine Grace";
            case 5: return "Notification (Not Activated)";
            case 6: return "Extended Grace";
            default: return "Unknown";
        }
    };

    bool foundWin = false;
    bool winActivated = false;
    for (const auto &p : winProducts) {
        foundWin = true;
        int status = p["LicenseStatus"].toInt();
        lines << "Product       : " + p["Name"].toString();
        lines << "LicenseStatus : " + licenseStatusText(status);
        if (status == 1) winActivated = true; // a genuine, activated OS license
    }

    if (foundWin) {
        lines << QString("Activation    : ") + (winActivated ? "Activated" : "Not Activated");
        overview.winActivation = winActivated ? "Activated" : "Not Activated";
    } else {
        lines << "Activation info: Could not find Windows license entry.";
        overview.winActivation = "Not Activated"; // do not claim activated when unknown
    }

    lines << "";
    CrashHandler::log(QString("Read Windows activation status in %1 ms").arg(stepTimer.restart()));

    // 6. Office Activation
    progressCallback(step++, total, "Checking Microsoft Office activation");
    lines << "=== Microsoft Office / 365 Activation ===";
    auto products = queryWmi("SELECT Name, LicenseStatus, PartialProductKey FROM SoftwareLicensingProduct WHERE PartialProductKey IS NOT NULL");
    bool foundOffice = false;
    bool officeActivated = false;
    for (const auto &p : products) {
        QString name = p["Name"].toString();
        if (name.contains("Office", Qt::CaseInsensitive) || name.contains("Microsoft 365", Qt::CaseInsensitive)) {
            foundOffice = true;
            int status = p["LicenseStatus"].toInt();
            lines << "Product       : " + name;
            lines << "LicenseStatus : " + licenseStatusText(status);
            // Office can expose several SKUs (e.g. a grace/trial entry alongside the
            // activated one). Treat Office as activated if ANY SKU is licensed.
            if (status == 1) officeActivated = true;
            lines << "----------------------------------------";
        }
    }
    if (foundOffice) {
        lines << QString("Activation    : ") + (officeActivated ? "Activated" : "Not Activated");
        overview.officeActivation = officeActivated ? "Activated" : "Not Activated";
    } else {
        lines << "Office Licensing: No Office-related licenses found.";
    }
    lines << "";
    CrashHandler::log(QString("Read Office activation status in %1 ms").arg(stepTimer.restart()));

    // 7. Network
    progressCallback(step++, total, "Reading network information");
    lines << "=== Network ===";
    // Win32_NetworkAdapterConfiguration (IPEnabled) is ~5x faster than enumerating
    // Win32_NetworkAdapter with the NetEnabled filter, and lists only active NICs.
    auto adapters = queryWmi("SELECT Description, MACAddress FROM Win32_NetworkAdapterConfiguration WHERE IPEnabled = True");
    if (adapters.isEmpty()) lines << "No active network adapters found.";

    // Names that indicate virtual / non-physical adapters we don't want to show.
    const QStringList virtualHints = {
        "virtual", "vmware", "virtualbox", "hyper-v", "loopback", "tunnel",
        "tap-", "tap ", "pseudo", "vpn", "wan miniport", "bluetooth"
    };
    for (const auto &nic : adapters) {
        QString desc = nic["Description"].toString();
        QString mac  = nic["MACAddress"].toString();
        lines << "Name        : " + desc;
        lines << "MAC Address : " + mac;
        lines << "Status      : Up";
        lines << "----------------------------------------";

        const QString low = desc.toLower();
        bool isVirtual = false;
        for (const QString &h : virtualHints)
            if (low.contains(h)) { isVirtual = true; break; }
        if (isVirtual || mac.isEmpty())
            continue;

        NetworkAdapterEntry ne;
        ne.name = desc;
        ne.mac = mac;
        ne.wireless = low.contains("wi-fi") || low.contains("wifi")
                   || low.contains("wireless") || low.contains("wlan")
                   || low.contains("802.11");
        overview.networkAdapters.append(ne);
    }
    emit cardReady("network", overview);
    lines << "";
    CrashHandler::log(QString("Read network information in %1 ms").arg(stepTimer.restart()));

    // 8. Display info is detected at the very end of runScan and pushed into the
    //    already-shown summary, so it never delays the rest of the info.

    // 9. Drivers — deferred. The check runs entirely on its own and its result
    //    is appended to the log after all other info is already shown. (Encryption
    //    status is read at the very end of runScan so it never gates this step.)
    progressCallback(step++, total, "Checking drivers");
    lines << "=== Drivers ===";
    lines << "Driver check runs in the background; results appear in the log.";
    lines << "";
    CrashHandler::log(QString("Read driver check placeholder in %1 ms").arg(stepTimer.restart()));

    // 10. Fast Startup
    progressCallback(step++, total, "Checking fast startup status");
    lines << "=== Fast Startup ===";
    #ifdef Q_OS_WIN
    QSettings settings("HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power", QSettings::NativeFormat);
    if (settings.contains("HiberbootEnabled")) {
        int val = settings.value("HiberbootEnabled").toInt();
        lines << "Status       : " + QString(val == 1 ? "On" : "Off");
    } else {
        lines << "Status       : Unknown";
    }
    #endif
    lines << "";
    CrashHandler::log(QString("Read fast startup status in %1 ms").arg(stepTimer.restart()));

    // Collect the encryption status now, right before the activation card is
    // emitted, so encryption and activation appear together. Its probe started at
    // scan begin and has had all of getWindowsInfo to finish, so this rarely
    // blocks; the BitLocker WMI provider can still be slow on first use, so cap
    // the wait so it never freezes the scan.
    #ifdef Q_OS_WIN
    if (m_encProc) {
        if (m_encProc->waitForFinished(3000)) {
            QFile ef(m_encryptionPath);
            if (ef.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString enc = QString::fromLatin1(ef.readAll()).trimmed();
                ef.close();
                if (enc == "Encrypted")         overview.encryptionStatus = "Encrypted";
                else if (enc == "NotEncrypted") overview.encryptionStatus = "Not Encrypted";
            }
        } else {
            CrashHandler::log("Encryption status check timed out after 3000 ms; leaving status unknown");
        }
    }
    CrashHandler::log(QString("Read encryption status in %1 ms").arg(stepTimer.restart()));
    #endif

    // Activation + encryption are both known now.
    emit cardReady("activation", overview);

    return lines.join("\n");
}

// Robust WMI numeric fields (Win32_* Size / StartingOffset / FreeSpace are often VT_I8/VT_UI8 or BSTR).
static qint64 wmiInt64(const QVariant &v)
{
    if (!v.isValid())
        return 0;
    bool ok = false;
    qint64 n = v.toLongLong(&ok);
    if (ok)
        return n;
    QString s = v.toString().trimmed();
    if (s.isEmpty() || s == QStringLiteral("N/A"))
        return 0;
    s.remove(QLatin1Char(','));
    n = s.toLongLong(&ok);
    return ok ? n : 0;
}

// IOCTL layout offsets sometimes differ slightly from WMI (alignment); merge within 1 MiB.
static bool takeWmiAtOffset(QMap<qint64, PartitionInfo> &wmiMap, qint64 offset, PartitionInfo *out)
{
    if (wmiMap.contains(offset)) {
        *out = wmiMap.take(offset);
        return true;
    }
    const qint64 alignTol = 4LL * 1048576LL; // 4 MiB — WMI vs IOCTL offsets can differ more on GPT
    qint64 bestKey = -1;
    qint64 bestDist = (std::numeric_limits<qint64>::max)();
    for (auto it = wmiMap.constBegin(); it != wmiMap.constEnd(); ++it) {
        const qint64 d = qAbs(it.key() - offset);
        if (d <= alignTol && d < bestDist) {
            bestDist = d;
            bestKey = it.key();
        }
    }
    if (bestKey >= 0) {
        *out = wmiMap.take(bestKey);
        return true;
    }
    return false;
}

#ifdef Q_OS_WIN
// Little-endian unsigned integer from a byte range (up to 8 bytes), clamped to qint64.
static qint64 leBytes(const BYTE *p, int count)
{
    quint64 v = 0;
    for (int i = count - 1; i >= 0; --i)
        v = (v << 8) | p[i];
    if (v > static_cast<quint64>((std::numeric_limits<qint64>::max)()))
        return (std::numeric_limits<qint64>::max)();
    return static_cast<qint64>(v);
}

// Parses a raw 512-byte SMART READ DATA sector (2-byte structure version
// followed by up to 30 attribute entries of 12 bytes) into the health struct.
// Shared by every ATA read path: legacy IOCTL, direct ATA pass-through and
// SAT pass-through for USB bridges.
static void parseAtaSmartAttributes(const BYTE *attrs, SmartHealthInfo &s)
{
    // [0]=id, [1..2]=flags, [3]=current, [4]=worst, [5..10]=raw
    for (int i = 0; i < 30; ++i) {
        const BYTE *e = attrs + 2 + i * 12;
        const BYTE id = e[0];
        if (id == 0)
            continue;
        const BYTE current = e[3];
        const qint64 raw = leBytes(e + 5, 6);
        switch (id) {
            case 0x05: s.reallocatedSectors = raw; break;
            case 0x09: s.powerOnHours = raw & 0xFFFFFFFFLL; break; // high bytes vendor-specific
            case 0x0C: s.powerCycles = raw; break;
            case 0xBE: // airflow temperature
            case 0xC2: // temperature (raw low byte in °C)
                if (s.temperatureC < 0) s.temperatureC = static_cast<int>(raw & 0xFF);
                break;
            case 0xC5: s.pendingSectors = raw; break;
            case 0xC6: s.uncorrectableSectors = raw; break;
            case 0xB1: // Samsung wear leveling count
            case 0xE7: // SSD life left
            case 0xE9: // media wearout indicator
                // Normalized current value = % life remaining on these.
                if (s.percentageUsed < 0 && current > 0 && current <= 100)
                    s.percentageUsed = 100 - current;
                break;
            case 0xF1: // total LBAs written (usually 512-byte units)
                if (raw > 0) s.hostWrittenTb = raw * 512.0 / 1e12;
                break;
        }
    }
    // A drive that answers SMART READ DATA but reports no defect attributes
    // still counts those as known-zero, so the UI can show explicit counts.
    if (s.reallocatedSectors < 0) s.reallocatedSectors = 0;
    if (s.pendingSectors < 0) s.pendingSectors = 0;
    if (s.uncorrectableSectors < 0) s.uncorrectableSectors = 0;
}

// One IOCTL_SCSI_PASS_THROUGH exchange: sends 'cdb' and transfers up to 512
// bytes in the given direction. Shared by the SAT paths and the USB-NVMe
// bridge vendor protocols below.
static bool scsiPassThrough(HANDLE h, const BYTE *cdb, UCHAR cdbLen,
                            void *data, DWORD dataLen, bool dataIn)
{
    struct SptBuffer {
        SCSI_PASS_THROUGH spt;
        ULONG pad;
        BYTE sense[32];
        BYTE data[512];
    } b = {};
    if (dataLen > sizeof(b.data) || cdbLen > sizeof(b.spt.Cdb))
        return false;

    b.spt.Length = sizeof(SCSI_PASS_THROUGH);
    b.spt.CdbLength = cdbLen;
    b.spt.SenseInfoLength = sizeof(b.sense);
    b.spt.DataIn = dataIn ? SCSI_IOCTL_DATA_IN : SCSI_IOCTL_DATA_OUT;
    b.spt.DataTransferLength = dataLen;
    b.spt.TimeOutValue = 5;
    b.spt.DataBufferOffset = offsetof(SptBuffer, data);
    b.spt.SenseInfoOffset = offsetof(SptBuffer, sense);
    memcpy(b.spt.Cdb, cdb, cdbLen);
    if (!dataIn && dataLen)
        memcpy(b.data, data, dataLen);

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH, &b, sizeof(b), &b, sizeof(b),
                         &ret, NULL))
        return false;
    if (b.spt.ScsiStatus != 0)
        return false;
    if (dataIn && dataLen)
        memcpy(data, b.data, dataLen);
    return true;
}

// Parses the 512-byte NVMe SMART / Health Information log (page 0x02) into
// the health struct. Shared by the native NVMe protocol query and the
// USB-NVMe bridge paths.
static void parseNvmeHealthLog(const BYTE *log, SmartHealthInfo &s,
                               bool &critical, bool &warning)
{
    const BYTE critWarn = log[0];
    const int tempK = log[1] | (log[2] << 8);
    s.temperatureC = (tempK > 0) ? tempK - 273 : -1;
    s.availableSpare  = log[3];
    const int spareThreshold = log[4];
    s.percentageUsed  = log[5];
    const qint64 dataUnitsWritten = leBytes(log + 48, 8);
    if (dataUnitsWritten > 0)
        s.hostWrittenTb = dataUnitsWritten * 512000.0 / 1e12;
    s.powerCycles     = leBytes(log + 112, 8);
    s.powerOnHours    = leBytes(log + 128, 8);
    s.unsafeShutdowns = leBytes(log + 144, 8);
    s.mediaErrors     = leBytes(log + 160, 8);

    if (critWarn & 0x01) { s.issues << "Available spare below threshold"; critical = true; }
    if (critWarn & 0x02) { s.issues << "Temperature outside limits"; warning = true; }
    if (critWarn & 0x04) { s.issues << "NVM subsystem reliability degraded"; critical = true; }
    if (critWarn & 0x08) { s.issues << "Media in read-only mode"; critical = true; }
    if (!critical && s.availableSpare >= 0 && spareThreshold > 0
            && s.availableSpare < spareThreshold) {
        s.issues << "Available spare below threshold";
        critical = true;
    }

    s.protocol = "NVMe";
    s.valid = true;
}

// USB bridges answer vendor CDBs unpredictably: a wrong-protocol response can
// be all zeros or stale garbage instead of an error. Only accept a health log
// whose fixed fields are physically plausible.
static bool nvmeLogLooksValid(const BYTE *log)
{
    bool nonZero = false;
    for (int i = 0; i < 512 && !nonZero; ++i)
        nonZero = log[i] != 0;
    if (!nonZero)
        return false;
    const int tempK = log[1] | (log[2] << 8);
    if (tempK < 233 || tempK > 400) // -40 °C … 127 °C
        return false;
    if (log[3] > 100) // available spare is a percentage
        return false;
    return true;
}

// SMART READ DATA via IOCTL_ATA_PASS_THROUGH. Reaches SATA drives whose
// driver rejects the legacy SMART_RCV_DRIVE_DATA ioctl, and some USB bridges.
static bool readSmartViaAtaPassThrough(HANDLE h, SmartHealthInfo &s)
{
    const DWORD bufLen = sizeof(ATA_PASS_THROUGH_EX) + 512;
    std::vector<BYTE> buf(bufLen, 0);
    auto *apt = reinterpret_cast<ATA_PASS_THROUGH_EX *>(buf.data());
    apt->Length = sizeof(ATA_PASS_THROUGH_EX);
    apt->AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    apt->DataTransferLength = 512;
    apt->TimeOutValue = 5;
    apt->DataBufferOffset = sizeof(ATA_PASS_THROUGH_EX);
    // Task file: SMART READ DATA (command 0xB0, feature 0xD0, LBA mid/high
    // must carry the SMART signature 0x4F/0xC2).
    apt->CurrentTaskFile[0] = 0xD0; // features
    apt->CurrentTaskFile[1] = 1;    // sector count
    apt->CurrentTaskFile[3] = 0x4F; // cylinder low
    apt->CurrentTaskFile[4] = 0xC2; // cylinder high
    apt->CurrentTaskFile[6] = 0xB0; // command

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH, buf.data(), bufLen,
                         buf.data(), bufLen, &ret, NULL) || ret < bufLen)
        return false;
    const BYTE *data = buf.data() + sizeof(ATA_PASS_THROUGH_EX);
    // An all-zero sector means the bridge swallowed the command silently.
    bool nonZero = false;
    for (int i = 0; i < 512 && !nonZero; ++i)
        nonZero = data[i] != 0;
    if (!nonZero)
        return false;
    parseAtaSmartAttributes(data, s);
    s.protocol = "ATA";
    s.valid = true;
    return true;
}

// SMART READ DATA wrapped in a SCSI "ATA PASS-THROUGH (16)" CDB (SAT). This
// is the standard route through USB-SATA bridges in external enclosures.
static bool readSmartViaSat(HANDLE h, SmartHealthInfo &s)
{
    BYTE cdb[16] = {};
    cdb[0] = 0x85;        // ATA PASS-THROUGH (16)
    cdb[1] = 4 << 1;      // protocol: PIO Data-In
    cdb[2] = 0x0E;        // T_DIR=in, BYT_BLOK=blocks, T_LENGTH=sector count
    cdb[4] = 0xD0;        // features: SMART READ DATA
    cdb[6] = 1;           // sector count
    cdb[10] = 0x4F;       // LBA mid — SMART signature
    cdb[12] = 0xC2;       // LBA high — SMART signature
    cdb[14] = 0xB0;       // command: SMART

    BYTE data[512] = {};
    if (!scsiPassThrough(h, cdb, sizeof(cdb), data, sizeof(data), true))
        return false;
    bool nonZero = false;
    for (int i = 0; i < 512 && !nonZero; ++i)
        nonZero = data[i] != 0;
    if (!nonZero)
        return false;
    parseAtaSmartAttributes(data, s);
    s.protocol = "ATA (USB/SAT)";
    s.valid = true;
    return true;
}

// Same SAT command in the 12-byte CDB form (A1h) — some USB-SATA bridges only
// implement this one and reject the 16-byte variant.
static bool readSmartViaSat12(HANDLE h, SmartHealthInfo &s)
{
    BYTE cdb[12] = {};
    cdb[0] = 0xA1;        // ATA PASS-THROUGH (12)
    cdb[1] = 4 << 1;      // protocol: PIO Data-In
    cdb[2] = 0x0E;        // T_DIR=in, BYT_BLOK=blocks, T_LENGTH=sector count
    cdb[3] = 0xD0;        // features: SMART READ DATA
    cdb[4] = 1;           // sector count
    cdb[6] = 0x4F;        // LBA mid — SMART signature
    cdb[7] = 0xC2;        // LBA high — SMART signature
    cdb[9] = 0xB0;        // command: SMART

    BYTE data[512] = {};
    if (!scsiPassThrough(h, cdb, sizeof(cdb), data, sizeof(data), true))
        return false;
    bool nonZero = false;
    for (int i = 0; i < 512 && !nonZero; ++i)
        nonZero = data[i] != 0;
    if (!nonZero)
        return false;
    parseAtaSmartAttributes(data, s);
    s.protocol = "ATA (USB/SAT)";
    s.valid = true;
    return true;
}

// NVMe health log through a Realtek RTL9210/9220 USB-NVMe bridge (vendor CDB
// 0xE4 — the translation smartmontools calls "sntrealtek"). The bridge caps
// transfers at 0x200 bytes, exactly one health log page.
static bool readNvmeSmartViaRealtek(HANDLE h, SmartHealthInfo &s,
                                    bool &critical, bool &warning)
{
    BYTE log[512] = {};
    BYTE cdb[16] = {};
    cdb[0] = 0xE4;                          // Realtek vendor NVMe pass-through
    cdb[1] = (BYTE)(sizeof(log) & 0xFF);    // transfer size, little-endian
    cdb[2] = (BYTE)(sizeof(log) >> 8);
    cdb[3] = 0x02;                          // NVMe opcode: Get Log Page
    cdb[4] = 0x02;                          // CDW10 low byte: health log id

    if (!scsiPassThrough(h, cdb, sizeof(cdb), log, sizeof(log), true))
        return false;
    if (!nvmeLogLooksValid(log))
        return false;
    parseNvmeHealthLog(log, s, critical, warning);
    s.protocol = "NVMe (USB)";
    return true;
}

// NVMe health log through an ASMedia ASM236x USB-NVMe bridge (vendor CDB
// 0xE6 — smartmontools "sntasmedia").
static bool readNvmeSmartViaAsmedia(HANDLE h, SmartHealthInfo &s,
                                    bool &critical, bool &warning)
{
    BYTE log[512] = {};
    const unsigned numd = sizeof(log) / 4 - 1; // CDW10 bits 31:16: dwords - 1
    BYTE cdb[16] = {};
    cdb[0] = 0xE6;                 // ASMedia vendor NVMe pass-through
    cdb[1] = 0x02;                 // NVMe opcode: Get Log Page
    cdb[3] = 0x02;                 // CDW10 low byte: health log id
    cdb[6] = (BYTE)(numd >> 8);    // CDW10 high word, big-endian
    cdb[7] = (BYTE)numd;

    if (!scsiPassThrough(h, cdb, sizeof(cdb), log, sizeof(log), true))
        return false;
    if (!nvmeLogLooksValid(log))
        return false;
    parseNvmeHealthLog(log, s, critical, warning);
    s.protocol = "NVMe (USB)";
    return true;
}

// NVMe health log through a JMicron JMS583 USB-NVMe bridge (smartmontools
// "sntjmicron"): a three-phase protocol tunnelled in ATA PASS-THROUGH (12)
// CDBs — send a 512-byte NVMe command block, DMA the data, read the reply.
static bool readNvmeSmartViaJmicron(HANDLE h, SmartHealthInfo &s,
                                    bool &critical, bool &warning)
{
    const quint32 kSignature = 0x454D564E; // 'NVME' (little-endian)
    // cdb[1]: [admin-command flag : 1][protocol : 7]
    const BYTE kAdmin = 0x80;
    const BYTE kProtoNvmCmd = 0x0, kProtoDmaIn = 0x2, kProtoResponse = 0xF;

    // Phase 1: send the NVMe admin command block.
    quint32 cmd[128] = {};
    cmd[0]  = kSignature;
    cmd[2]  = 0x02;                          // opcode: Get Log Page
    cmd[3]  = 0xFFFFFFFFu;                   // NSID: broadcast
    cmd[12] = 0x02 | ((512u / 4 - 1) << 16); // CDW10: health log id + NUMD
    BYTE cdb[12] = {};
    cdb[0] = 0xA1;
    cdb[1] = kAdmin | kProtoNvmCmd;
    cdb[4] = 2;                              // payload length 512, big-endian 24-bit
    if (!scsiPassThrough(h, cdb, sizeof(cdb), cmd, sizeof(cmd), false))
        return false;

    // Phase 2: DMA the log page in.
    BYTE log[512] = {};
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA1;
    cdb[1] = kAdmin | kProtoDmaIn;
    cdb[4] = 2;
    if (!scsiPassThrough(h, cdb, sizeof(cdb), log, sizeof(log), true))
        return false;

    // Phase 3: fetch the response block; verify signature and NVMe status.
    quint32 reply[128] = {};
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA1;
    cdb[1] = kAdmin | kProtoResponse;
    cdb[4] = 2;
    if (!scsiPassThrough(h, cdb, sizeof(cdb), reply, sizeof(reply), true))
        return false;
    if (reply[0] != kSignature || (reply[5] >> 17) != 0)
        return false;

    if (!nvmeLogLooksValid(log))
        return false;
    parseNvmeHealthLog(log, s, critical, warning);
    s.protocol = "NVMe (USB)";
    return true;
}

// True when the physical drive sits behind a USB bridge (UASP enclosures
// report themselves as SCSI in WMI, but the storage stack knows the bus).
static bool isUsbBusType(HANDLE h)
{
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    BYTE buf[1024] = {};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                         buf, sizeof(buf), &ret, NULL)
        || ret < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return false;
    return reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(buf)->BusType == BusTypeUsb;
}

// Whether Windows currently holds the whole physical disk Offline. Offline
// disks still answer this IOCTL, so a failure to open/query just reports online.
static bool readDiskIsOffline(const QString &diskNumber)
{
    if (diskNumber.isEmpty())
        return false;
    QString path = QString("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    HANDLE h = CreateFileW((LPCWSTR)path.toStdWString().c_str(),
                           GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    GET_DISK_ATTRIBUTES ga = {};
    DWORD ret = 0;
    bool offline = false;
    if (DeviceIoControl(h, IOCTL_DISK_GET_DISK_ATTRIBUTES, NULL, 0, &ga, sizeof(ga), &ret, NULL))
        offline = (ga.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0;
    CloseHandle(h);
    return offline;
}

// SMART health via IOCTLs — NVMe health log (page 0x02) first, then the ATA
// attribute table through four routes (legacy SMART ioctl, direct ATA
// pass-through, SAT in 16- and 12-byte CDB form), and finally the vendor
// NVMe pass-through protocols of the common USB-NVMe enclosure bridges
// (Realtek RTL9210, JMicron JMS583, ASMedia ASM236x). Devices that expose
// none of these (most USB flash sticks, some RAID controllers) come back
// with valid == false and the UI says so honestly.
static SmartHealthInfo readSmartHealth(const QString &diskNumber, const QString &model = QString())
{
    SmartHealthInfo s;
    if (diskNumber.isEmpty())
        return s;

    QString path = QString("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    // SMART_RCV_DRIVE_DATA wants read+write access; fall back to read-only,
    // which is still enough for the NVMe protocol query.
    HANDLE h = CreateFileW((LPCWSTR)path.toStdWString().c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW((LPCWSTR)path.toStdWString().c_str(),
                        GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    }
    if (h == INVALID_HANDLE_VALUE)
        return s;

    bool critical = false;
    bool warning = false;

    // ── NVMe: SMART / health information log ──
    {
        const DWORD dataLen = 512;
        const DWORD bufLen = FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters)
                             + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + dataLen;
        std::vector<BYTE> buf(bufLen, 0);

        auto *q  = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf.data());
        auto *pd = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(q->AdditionalParameters);
        q->PropertyId = StorageDeviceProtocolSpecificProperty;
        q->QueryType  = PropertyStandardQuery;
        pd->ProtocolType = ProtocolTypeNvme;
        pd->DataType = NVMeDataTypeLogPage;
        pd->ProtocolDataRequestValue = 0x02; // health info log page
        pd->ProtocolDataRequestSubValue = 0;
        pd->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
        pd->ProtocolDataLength = dataLen;

        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            buf.data(), bufLen, buf.data(), bufLen, &ret, NULL)) {
            auto *desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buf.data());
            const STORAGE_PROTOCOL_SPECIFIC_DATA &out = desc->ProtocolSpecificData;
            if (out.ProtocolDataLength >= 176 &&
                sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) - sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)
                    + out.ProtocolDataOffset + out.ProtocolDataLength <= bufLen) {
                const BYTE *log = reinterpret_cast<const BYTE*>(&out) + out.ProtocolDataOffset;
                parseNvmeHealthLog(log, s, critical, warning);
            }
        }
    }

    // ── ATA/SATA: SMART READ ATTRIBUTES (legacy ioctl) ──
    if (!s.valid) {
        SENDCMDINPARAMS in = {};
        in.cBufferSize = READ_ATTRIBUTE_BUFFER_SIZE;
        in.irDriveRegs.bFeaturesReg    = READ_ATTRIBUTES;
        in.irDriveRegs.bSectorCountReg = 1;
        in.irDriveRegs.bSectorNumberReg = 1;
        in.irDriveRegs.bCylLowReg  = SMART_CYL_LOW;
        in.irDriveRegs.bCylHighReg = SMART_CYL_HI;
        in.irDriveRegs.bCommandReg = SMART_CMD;

        const DWORD outLen = sizeof(SENDCMDOUTPARAMS) - 1 + READ_ATTRIBUTE_BUFFER_SIZE;
        std::vector<BYTE> out(outLen, 0);
        DWORD ret = 0;
        if (DeviceIoControl(h, SMART_RCV_DRIVE_DATA, &in, sizeof(in) - 1,
                            out.data(), outLen, &ret, NULL)) {
            parseAtaSmartAttributes(
                reinterpret_cast<SENDCMDOUTPARAMS*>(out.data())->bBuffer, s);
            s.protocol = "ATA";
            s.valid = true;
        }
    }

    // ── ATA via direct pass-through (drivers that reject the legacy ioctl) ──
    if (!s.valid)
        readSmartViaAtaPassThrough(h, s);

    // ── ATA via SAT (USB-SATA bridges: external drives / enclosures) ──
    if (!s.valid)
        readSmartViaSat(h, s);

    // ── ATA via SAT, 12-byte CDB (bridges that reject the 16-byte form) ──
    if (!s.valid)
        readSmartViaSat12(h, s);

    // ── USB-NVMe enclosure bridges: vendor NVMe pass-through ──
    s.usbBus = isUsbBusType(h);
    if (!s.valid && s.usbBus) {
        // The bridge chip usually shows in the SCSI product string (e.g.
        // "Realtek RTL9210 NVME"), so try the hinted protocol first; JMicron
        // enclosures typically pass the NVMe drive's own model through, so
        // they get no hint and lead the blind order.
        const QString m = model.toLower();
        if (m.contains("rtl9") || m.contains("realtek")) {
            readNvmeSmartViaRealtek(h, s, critical, warning)
                || readNvmeSmartViaJmicron(h, s, critical, warning)
                || readNvmeSmartViaAsmedia(h, s, critical, warning);
        } else if (m.contains("asmt") || m.contains("asmedia") || m.contains("asm2")) {
            readNvmeSmartViaAsmedia(h, s, critical, warning)
                || readNvmeSmartViaJmicron(h, s, critical, warning)
                || readNvmeSmartViaRealtek(h, s, critical, warning);
        } else {
            readNvmeSmartViaJmicron(h, s, critical, warning)
                || readNvmeSmartViaRealtek(h, s, critical, warning)
                || readNvmeSmartViaAsmedia(h, s, critical, warning);
        }
    }

    CloseHandle(h);

    if (!s.valid)
        return s;

    // ── shared verdict rules ──
    if (s.percentageUsed >= 95) {
        s.issues << QString("SSD wear at %1%").arg(s.percentageUsed);
        critical = true;
    } else if (s.percentageUsed >= 80) {
        s.issues << QString("SSD wear at %1%").arg(s.percentageUsed);
        warning = true;
    }

    const qint64 badSectors = (std::max)({s.reallocatedSectors, s.pendingSectors, s.uncorrectableSectors});
    if (badSectors > 50) {
        s.issues << QString("Bad sectors: %1 reallocated, %2 pending, %3 uncorrectable")
                        .arg((std::max)(s.reallocatedSectors, 0LL))
                        .arg((std::max)(s.pendingSectors, 0LL))
                        .arg((std::max)(s.uncorrectableSectors, 0LL));
        critical = true;
    } else if (badSectors > 0) {
        s.issues << QString("Bad sectors: %1 reallocated, %2 pending, %3 uncorrectable")
                        .arg((std::max)(s.reallocatedSectors, 0LL))
                        .arg((std::max)(s.pendingSectors, 0LL))
                        .arg((std::max)(s.uncorrectableSectors, 0LL));
        warning = true;
    }

    if (s.mediaErrors > 0) {
        s.issues << QString("Media errors: %1").arg(s.mediaErrors);
        warning = true;
    }
    if (s.temperatureC >= 70) {
        s.issues << QString("High temperature: %1 °C").arg(s.temperatureC);
        warning = true;
    }

    s.verdict = critical ? "Critical" : (warning ? "Warning" : "Good");

    // Overall health score: remaining SSD life as the base (100 for HDDs /
    // drives without a wear indicator), minus deductions for defects.
    {
        int score = 100;
        if (s.percentageUsed >= 0)
            score = 100 - qMin(s.percentageUsed, 100);

        auto deduct = [&](qint64 count, int perUnit, int cap) {
            if (count > 0)
                score -= static_cast<int>(qMin<qint64>(count * perUnit, cap));
        };
        deduct(s.reallocatedSectors, 1, 30);   // remapped but stable
        deduct(s.pendingSectors, 2, 30);       // unstable — worse sign
        deduct(s.uncorrectableSectors, 2, 30);
        deduct(s.mediaErrors, 1, 20);
        deduct(s.unsafeShutdowns > 100 ? s.unsafeShutdowns / 100 : 0, 1, 5);

        if (s.availableSpare >= 0 && s.availableSpare < 100)
            score -= (100 - s.availableSpare) / 5; // spare consumption is an early wear sign

        // Hard caps for states that make the drive untrustworthy regardless of score.
        if (critical)
            score = qMin(score, 30);

        s.healthPercent = qBound(0, score, 100);
    }
    return s;
}
#endif // Q_OS_WIN

// Enumerates physical disks and partitions (WMI + IOCTL layout), determines the
// system disk, offline state, WinRE status and SMART health for each disk.
QList<StorageDisk> HardwareModel::getStorageDetails() {
    QList<StorageDisk> disks;
    QString systemDrive = "C:";
    QString systemDiskNum;

    auto osInfo = queryWmi("SELECT SystemDrive FROM Win32_OperatingSystem");
    if (!osInfo.isEmpty()) {
        QString detected = osInfo.first()["SystemDrive"].toString().trimmed();
        if (!detected.isEmpty()) {
            if (detected.size() >= 2 && detected[1] == ':') {
                systemDrive = detected.left(2).toUpper();
            } else {
                systemDrive = detected;
            }
        }
    }

    // Resolve system physical disk from SystemDrive through WMI associations:
    // LogicalDisk (C:) -> DiskPartition -> DiskDrive (PHYSICALDRIVE#).
    if (!systemDrive.isEmpty()) {
        QString logicalToPartQuery =
            QString("ASSOCIATORS OF {Win32_LogicalDisk.DeviceID='%1'} WHERE AssocClass = Win32_LogicalDiskToPartition")
                .arg(systemDrive);
        auto sysParts = queryWmi(logicalToPartQuery);

        for (const auto &part : sysParts) {
            QString partId = part["DeviceID"].toString();
            if (partId.isEmpty()) {
                continue;
            }

            QString partToDiskQuery =
                QString("ASSOCIATORS OF {Win32_DiskPartition.DeviceID='%1'} WHERE AssocClass = Win32_DiskDriveToDiskPartition")
                    .arg(partId);
            partToDiskQuery.replace("\\", "\\\\");
            auto sysDisks = queryWmi(partToDiskQuery);
            if (sysDisks.isEmpty()) {
                continue;
            }

            QString sysDiskDeviceId = sysDisks.first()["DeviceID"].toString(); // \\.\PHYSICALDRIVE#
            QRegularExpression sysRe("PHYSICALDRIVE(\\d+)");
            QRegularExpressionMatch sysMatch = sysRe.match(sysDiskDeviceId);
            if (sysMatch.hasMatch()) {
                systemDiskNum = sysMatch.captured(1);
                break;
            }
        }
    }

    // 1. Get Physical Disks
    auto drives = queryWmi("SELECT DeviceID, Model, MediaType, Size, InterfaceType FROM Win32_DiskDrive");
    for (const auto &d : drives) {
        StorageDisk disk;
        disk.deviceID = d["DeviceID"].toString(); // e.g. \\.\PHYSICALDRIVE0
        disk.model = d["Model"].toString();
        disk.mediaType = d["MediaType"].toString();
        disk.interfaceType = d["InterfaceType"].toString();
        disk.size = wmiInt64(d["Size"]);
        disk.isEmpty = false;
        disk.isSystem = false;

        // Extract disk number
        QString diskNumStr;
        QRegularExpression re("PHYSICALDRIVE(\\d+)");
        QRegularExpressionMatch match = re.match(disk.deviceID);
        if (match.hasMatch()) {
            diskNumStr = match.captured(1);
            disk.diskNumber = diskNumStr;
            disk.deviceID = disk.diskNumber; // Store just the number for display
        }

        if (!systemDiskNum.isEmpty() && disk.diskNumber == systemDiskNum) {
            disk.isSystem = true;
        }

        #ifdef Q_OS_WIN
        disk.smart = readSmartHealth(disk.diskNumber, disk.model);
        disk.isOffline = readDiskIsOffline(disk.diskNumber);
        #endif

        // 2. Find associated partitions
        // We use a hybrid approach: WMI for logical mapping, DeviceIoControl for accurate partition table

        QMap<qint64, PartitionInfo> wmiMap;

        QString partQuery = QString("ASSOCIATORS OF {Win32_DiskDrive.DeviceID='%1'} WHERE AssocClass = Win32_DiskDriveToDiskPartition").arg(d["DeviceID"].toString());
        partQuery.replace("\\", "\\\\");

        auto partitions = queryWmi(partQuery);
        for (const auto &part : partitions) {
            PartitionInfo info;
            info.size = wmiInt64(part["Size"]);
            info.physicalSize = info.size;
            qint64 offset = wmiInt64(part["StartingOffset"]);
            info.startingOffset = offset;

            if (part.contains("Index")) {
                info.partitionNum = QString::number(part["Index"].toInt() + 1);
            }

            // 3. Find associated logical disks (volumes)
            QString logQuery = QString("ASSOCIATORS OF {Win32_DiskPartition.DeviceID='%1'} WHERE AssocClass = Win32_LogicalDiskToPartition").arg(part["DeviceID"].toString());
            auto logicals = queryWmi(logQuery);

            if (!logicals.isEmpty()) {
                auto log = logicals.first();
                info.deviceID = log["DeviceID"].toString(); // C:, D:
                info.volumeName = log["VolumeName"].toString();
                info.fileSystem = log["FileSystem"].toString();

                // Use QStorageInfo for accurate space
                QStorageInfo storage(info.deviceID);
                if (storage.isValid() && storage.isReady()) {
                    info.size = storage.bytesTotal();
                    qint64 freeB = storage.bytesFree();
                    if (freeB < 0)
                        freeB = storage.bytesAvailable();
                    info.freeSpace = freeB;
                } else {
                    info.freeSpace = wmiInt64(log["FreeSpace"]);
                    // Fallback to LogicalDisk size if available, otherwise it keeps Partition Size
                    if (log.contains("Size")) {
                        info.size = wmiInt64(log["Size"]);
                    }
                }

                info.isLogical = true;

                // Check if this partition hosts the Windows system drive.
                if (info.deviceID.startsWith(systemDrive, Qt::CaseInsensitive)) {
                    disk.isSystem = true;
                }
            } else {
                info.deviceID = "Partition " + info.partitionNum; // Fallback name
                info.fileSystem = "Unknown"; // Likely recovery or system partition
                info.freeSpace = 0;
                info.isLogical = false;
            }

            wmiMap.insert(offset, info);
        }

        bool usedIoctl = false;
        #ifdef Q_OS_WIN
        // Read partition layout without requiring admin — matches Disk Management more often than WMI-only.
        if (!diskNumStr.isEmpty()) {
            QString path = QString("\\\\.\\PhysicalDrive%1").arg(diskNumStr);
            HANDLE hDevice = CreateFileW((LPCWSTR)path.toStdWString().c_str(),
                                         GENERIC_READ,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         NULL,
                                         OPEN_EXISTING,
                                         0,
                                         NULL);

            if (hDevice != INVALID_HANDLE_VALUE) {
                const int buffSize = 32768;
                QByteArray buffer(buffSize, 0);
                DWORD bytesReturned;

                BOOL result = DeviceIoControl(hDevice,
                                              IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                                              NULL, 0,
                                              buffer.data(), buffSize,
                                              &bytesReturned,
                                              NULL);

                if (result) {
                    usedIoctl = true;
                    DRIVE_LAYOUT_INFORMATION_EX* layout = (DRIVE_LAYOUT_INFORMATION_EX*)buffer.data();

                    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
                        PARTITION_INFORMATION_EX& part = layout->PartitionEntry[i];
                        if (part.PartitionLength.QuadPart > 0) {
                            PartitionInfo info;
                            info.size = part.PartitionLength.QuadPart;
                            info.physicalSize = info.size;
                            info.partitionNum = QString::number(part.PartitionNumber);

                            qint64 offset = part.StartingOffset.QuadPart;
                            info.startingOffset = offset;

                            PartitionInfo wmiInfo;
                            if (takeWmiAtOffset(wmiMap, offset, &wmiInfo)) {
                                info.deviceID = wmiInfo.deviceID;
                                info.volumeName = wmiInfo.volumeName;
                                info.fileSystem = wmiInfo.fileSystem;
                                info.freeSpace = wmiInfo.freeSpace;
                                info.isLogical = wmiInfo.isLogical;

                                // Use the accurate filesystem size if logical
                                if (info.isLogical) {
                                    info.size = wmiInfo.size;
                                }

                                // Re-check system drive flag just in case
                                if (info.deviceID.startsWith(systemDrive, Qt::CaseInsensitive)) {
                                    disk.isSystem = true;
                                }
                            } else {
                                info.deviceID = "Partition " + info.partitionNum;
                                info.fileSystem = "Unknown";
                                info.freeSpace = 0;
                                info.isLogical = false;
                            }

                            disk.partitions.append(info);
                        }
                    }
                }
                CloseHandle(hDevice);
            }
        }
        #endif

        if (!usedIoctl) {
            // Fallback to WMI — key is startingOffset
            for (auto it = wmiMap.begin(); it != wmiMap.end(); ++it) {
                PartitionInfo p = it.value();
                p.startingOffset = it.key();
                if (p.physicalSize == 0) p.physicalSize = p.size;
                disk.partitions.append(p);
            }
        }

        if (disk.partitions.isEmpty()) {
            disk.isEmpty = true;
        }

        qint64 allocated = 0;
        for (const auto &p : disk.partitions) {
            const qint64 plen = (p.physicalSize > 0) ? p.physicalSize : p.size;
            if (plen > 0)
                allocated += plen;
        }
        if (disk.size > 0 && allocated < disk.size)
            disk.unallocatedSpace = disk.size - allocated;
        else
            disk.unallocatedSpace = 0;

        std::sort(disk.partitions.begin(), disk.partitions.end(),
                  [](const PartitionInfo &a, const PartitionInfo &b) {
                      return a.startingOffset < b.startingOffset;
                  });

        disks.append(disk);
    }

    #ifdef Q_OS_WIN
    // WinRE (Windows Recovery) status — a whole-OS property, shown on the system
    // disk. The hosting disk (if different) is also tagged so the user sees where
    // WinRE physically lives.
    {
        QString winReDisk;
        const bool winReEnabled = readWinReInfo(&winReDisk);
        QString winReLocation;
        QProcess infoProcess;
        infoProcess.start("reagentc", {"/info"});
        if (infoProcess.waitForFinished(8000)) {
            const QString out = QString::fromLocal8Bit(infoProcess.readAllStandardOutput());
            const QString marker = "\\\\?\\GLOBALROOT";
            for (const QString &line : out.split(QRegularExpression("[\\r\\n]+"))) {
                const int start = line.indexOf(marker, 0, Qt::CaseInsensitive);
                if (start >= 0 && line.contains("harddisk", Qt::CaseInsensitive)) {
                    winReLocation = line.mid(start).trimmed();
                    break;
                }
            }
        }
        for (StorageDisk &disk : disks) {
            if (disk.isSystem)
                fillWinReDiagnostics(disk, winReEnabled, winReLocation);
            else if (winReEnabled && disk.diskNumber == winReDisk)
                disk.winReStatus = "Enabled";
        }
    }
    #endif

    return disks;
}

// Motherboard (Win32_BaseBoard) and BIOS (Win32_BIOS) details as key/value pairs.
QMap<QString, QString> HardwareModel::getMotherboardInfo() {
    QMap<QString, QString> info;

    auto boards = queryWmi("SELECT Manufacturer, Product, SerialNumber, Version FROM Win32_BaseBoard");
    for (const auto &b : boards) {
        info["Motherboard Manufacturer"] = b["Manufacturer"].toString();
        info["Motherboard Product"] = b["Product"].toString();
        info["Motherboard Serial Number"] = b["SerialNumber"].toString();
        info["Motherboard Version"] = b["Version"].toString();
    }

    auto bios = queryWmi("SELECT Manufacturer, SMBIOSBIOSVersion, ReleaseDate FROM Win32_BIOS");
    for (const auto &b : bios) {
        info["BIOS Manufacturer"] = b["Manufacturer"].toString();
        info["BIOS Version"] = b["SMBIOSBIOSVersion"].toString();
        info["BIOS Release Date"] = b["ReleaseDate"].toString();
    }

    // OA3 product key embedded in the firmware (OEM machines). Empty on
    // self-built PCs and volume-licensed hardware.
    auto oa3 = queryWmi("SELECT OA3xOriginalProductKey FROM SoftwareLicensingService");
    if (!oa3.isEmpty()) {
        const QString key = oa3.first()["OA3xOriginalProductKey"].toString().trimmed();
        if (!key.isEmpty())
            info["BIOS Product Key"] = key;
    }

    return info;
}

// Laptop: Win32_Battery plus design/full-charge capacity and cycle count parsed
// from the powercfg HTML report. Desktop: a single "Desktop" entry.
QList<BatteryInfo> HardwareModel::getBatteryDetails() {
    QList<BatteryInfo> list;

    // WMI Battery
    auto batteries = queryWmi("SELECT Name, BatteryStatus, EstimatedChargeRemaining FROM Win32_Battery");

    if (!batteries.isEmpty()) {
        // --- LAPTOP CASE ---
        BatteryInfo sysInfo;
        sysInfo.title = "System Type";
        sysInfo.data.append(QPair<QString, QString>("Type", "Laptop / Portable"));
        list.append(sysInfo);

        for (const auto &b : batteries) {
            BatteryInfo info;
            info.title = "Battery (WMI)";
            info.data.append(QPair<QString, QString>("Name", b["Name"].toString()));
            info.data.append(QPair<QString, QString>("Estimated Charge", b["EstimatedChargeRemaining"].toString() + "%"));
            list.append(info);
        }

        // Powercfg Battery Report (already started in parallel at scan begin).
        #ifdef Q_OS_WIN
        QString reportPath = m_batteryReportPath.isEmpty()
            ? QDir::temp().filePath("battery-report.html") : m_batteryReportPath;
        int reportExit = -1;
        if (m_batteryReportProc) {
            m_batteryReportProc->waitForFinished(-1); // usually already done
            reportExit = m_batteryReportProc->exitCode();
        } else {
            QProcess p;
            p.start("powercfg", QStringList() << "/batteryreport" << "/output" << reportPath);
            p.waitForFinished();
            reportExit = p.exitCode();
        }

        if (reportExit == 0 && QFile::exists(reportPath)) {
            QFile file(reportPath);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString html = file.readAll();
                file.close();

                // Restrict parsing to the "Installed batteries" section of the report.
                // History/capacity tables repeat the same labels and inflate the battery count.
                QString section = html;
                {
                    int start = html.indexOf("Installed batteries", 0, Qt::CaseInsensitive);
                    if (start != -1) {
                        int nextH2 = html.indexOf("<h2", start + 20, Qt::CaseInsensitive);
                        section = html.mid(start, (nextH2 != -1) ? nextH2 - start : -1);
                    }
                }

                // In the "Installed batteries" table each battery is a COLUMN, not a separate block.
                // The header row lists "BATTERY 1", "BATTERY 2", ... and every field row
                // (NAME, MANUFACTURER, DESIGN CAPACITY, ...) has one value <td> per battery:
                //   <tr><td><span class="label">NAME</span></td><td>val1</td><td>val2</td></tr>
                // So we read each field by COLUMN INDEX (battery 1 = first value cell, etc.).

                // Returns the list of value cells (label cell stripped) for a field row,
                // or an empty list if the label is not present.
                auto rowValueCells = [&](const QString &label) -> QStringList {
                    QRegularExpression rowRe(
                        "<tr[^>]*>(?:(?!</tr>).)*?" + QRegularExpression::escape(label) +
                        "(?:(?!</tr>).)*?</tr>",
                        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                    auto rowMatch = rowRe.match(section);
                    if (!rowMatch.hasMatch()) return QStringList();

                    QRegularExpression cellRe(
                        "<td[^>]*>(.*?)</td>",
                        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
                    QStringList cells;
                    auto cit = cellRe.globalMatch(rowMatch.captured(0));
                    while (cit.hasNext()) {
                        QString v = cit.next().captured(1);
                        v.remove(QRegularExpression("<[^>]+>")); // strip inner tags
                        cells.append(v.trimmed());
                    }
                    if (!cells.isEmpty()) cells.removeFirst(); // drop the label cell
                    return cells;
                };

                // Determine the battery count from the number of value columns in a real
                // field row (more reliable than parsing the "BATTERY n" header text).
                int numPcBatteries = 0;
                for (const QString &probe : {QStringLiteral("NAME"),
                                             QStringLiteral("DESIGN CAPACITY"),
                                             QStringLiteral("MANUFACTURER")}) {
                    int c = rowValueCells(probe).size();
                    if (c > 0) { numPcBatteries = c; break; }
                }
                if (numPcBatteries == 0) numPcBatteries = 1;

                // Return the value for the given field label and 0-based battery column index.
                auto getCell = [&](const QString &label, int col) -> QString {
                    const QStringList cells = rowValueCells(label);
                    return (col < cells.size()) ? cells[col] : QString();
                };

                auto getNumCell = [&](const QString &label, int col) -> qint64 {
                    QString v = getCell(label, col);
                    QRegularExpression numRe("([\\d,]+)");
                    auto m = numRe.match(v);
                    return m.hasMatch() ? m.captured(1).replace(",", "").toLongLong() : 0;
                };

                for (int bi = 0; bi < numPcBatteries; bi++) {
                    BatteryInfo info;
                    info.title = QString("Battery Health (powercfg) #%1").arg(bi + 1);

                    QString manufacturer = getCell("MANUFACTURER", bi);
                    QString serialNum    = getCell("SERIAL NUMBER", bi);
                    QString chemistry    = getCell("CHEMISTRY", bi);
                    QString cycleCount   = getCell("CYCLE COUNT", bi);

                    qint64 designCap = getNumCell("DESIGN CAPACITY", bi);
                    qint64 fullCap   = getNumCell("FULL CHARGE CAPACITY", bi);

                    auto isBlank = [](const QString &s) { return s.isEmpty() || s == "-"; };

                    if (!isBlank(manufacturer)) info.data.append(QPair<QString,QString>("Manufacturer", manufacturer));
                    if (!isBlank(serialNum))    info.data.append(QPair<QString,QString>("Serial Number", serialNum));
                    if (!isBlank(chemistry))    info.data.append(QPair<QString,QString>("Chemistry", chemistry));

                    if (designCap > 0 && fullCap > 0) {
                        double health = static_cast<double>(fullCap) / designCap * 100.0;
                        info.data.append(QPair<QString,QString>("Design Capacity",      QString::number(designCap) + " mWh"));
                        info.data.append(QPair<QString,QString>("Full Charge Capacity", QString::number(fullCap)   + " mWh"));
                        info.data.append(QPair<QString,QString>("Health",               QString::number(health, 'f', 1) + "%"));
                    }
                    if (!isBlank(cycleCount)) info.data.append(QPair<QString,QString>("Cycle Count", cycleCount));

                    if (info.data.isEmpty())
                        info.data.append(QPair<QString,QString>("Info", "Could not parse details from report"));

                    list.append(info);
                }
            }
            QFile::remove(reportPath);
        }
        #endif

    } else {
        // --- DESKTOP CASE ---
        BatteryInfo sysInfo;
        sysInfo.title = "System Power";
        sysInfo.data.append(QPair<QString, QString>("Type", "Desktop"));

        // Try to get Power Supply info
        // Note: Win32_PowerSupply is often empty or generic on many desktops.
        // We will try to get what we can.
        auto psuList = queryWmi("SELECT Name, DeviceID, Description, Model, MaxPowerOutput FROM Win32_PowerSupply");

        if (!psuList.isEmpty()) {
            for (const auto &psu : psuList) {
                QString name = psu["Name"].toString();
                QString model = psu["Model"].toString();
                QString desc = psu["Description"].toString();
                QVariant maxPower = psu["MaxPowerOutput"];

                if (!name.isEmpty()) sysInfo.data.append(QPair<QString, QString>("PSU Name", name));
                if (!model.isEmpty()) sysInfo.data.append(QPair<QString, QString>("PSU Model", model));
                if (!desc.isEmpty() && desc != name) sysInfo.data.append(QPair<QString, QString>("PSU Description", desc));

                if (maxPower.isValid() && !maxPower.isNull()) {
                     // MaxPowerOutput is in milliwatts
                     double watts = maxPower.toLongLong() / 1000.0;
                     if (watts > 0) {
                        sysInfo.data.append(QPair<QString, QString>("Max Power Output", QString::number(watts) + " W"));
                     }
                }
            }
        } else {
            // Fallback: Try to estimate or just show a message
            // Since Win32_PowerSupply is unreliable, we can't easily get the wattage via software on standard ATX PSUs
            // without specialized hardware sensors (like Corsair Link, etc.) which are proprietary.
            sysInfo.data.append(QPair<QString, QString>("PSU Info", "Standard PSU (Wattage not reported by OS)"));
            sysInfo.data.append(QPair<QString, QString>("Note", "Most desktop power supplies do not report data to Windows."));
        }

        list.append(sysInfo);
    }

    return list;
}
