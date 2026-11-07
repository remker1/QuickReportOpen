// =============================================================================
// HardwareModel.h - hardware / disk data model and Windows system operations.
//
// Two roles live here:
//   1. Plain data structs (GpuEntry ... BatteryInfo) describing what a scan found.
//      They are passed between the worker thread and the GUI via Qt signals, so
//      they must stay copyable value types (registered in main.cpp).
//   2. HardwareModel (QObject): runs on a worker thread, queries WMI/PowerShell/
//      DeviceIoControl, emits progressive results, and exposes static helpers for
//      destructive disk operations (clear, extend, create volume, eject) plus the
//      system-disk clone engine with progress/cancel support.
//
// All blocking members must be called OFF the GUI thread.
// =============================================================================
#ifndef HARDWAREMODEL_H
#define HARDWAREMODEL_H

#include <QObject>
#include <QString>
#include <QList>
#include <QMap>
#include <QVariant>
#include <atomic>
#include <functional>

class QProcess;

// One graphics adapter: display name and dedicated video memory in MiB.
struct GpuEntry {
    QString name;
    qint64 vramMb = 0;
};

// One connected display: resolution in pixels and refresh rate in Hz.
struct DisplayEntry {
    int width = 0;
    int height = 0;
    int refreshRate = 0;
};

// One physical network adapter (wired or Wi-Fi) with its MAC address.
struct NetworkAdapterEntry {
    QString name;
    QString mac;
    bool wireless = false;
};

// One populated memory module (DIMM).
struct RamStickEntry {
    qint64 sizeBytes = 0;
    QString manufacturer;
    int speedMhz = 0;
    QString slot;   // physical slot label, e.g. "DIMM A" / "ChannelA-DIMM0"
    QString type;   // "DDR4", "DDR5", etc.
};

// Aggregated data shown on the System Info cards (model, OS, CPU, GPU, RAM,
// displays, network, encryption). Filled progressively during the scan.
struct SystemSummaryData {
    QString systemModel;
    QString osName;
    QString winActivation;
    QString officeActivation; // "Activated", "Not Activated", or "" if not found

    QString cpuName;
    int cpuCores = 0;
    int cpuThreads = 0;

    QList<GpuEntry> gpus;
    QStringList npus;               // NPU / AI accelerator names (empty if none)

    qint64 totalRamBytes = 0;
    QList<qint64> ramStickSizes; // each stick capacity in bytes
    QString ramType;             // "DDR4", "DDR5", etc.
    int ramSpeedMhz = 0;
    QList<RamStickEntry> ramSticks; // per-populated-slot detail
    int ramSlotsTotal = 0;          // total physical memory slots

    QList<DisplayEntry> displays;
    bool hasTouch = false;          // touchscreen digitizer present
    bool displayReady = false;      // false while the display step is still running

    QList<NetworkAdapterEntry> networkAdapters;
    QString encryptionStatus; // "Encrypted", "Not Encrypted", or "" if unknown
};

// One partition/volume on a disk, including raw layout numbers used to draw
// the proportional partition bar in the Storage tab.
struct PartitionInfo {
    QString deviceID;       // e.g., "C:" or "Partition #1"
    QString volumeName;     // e.g., "Local Disk"
    QString fileSystem;     // e.g., "NTFS"
    QString partitionNum;   // e.g., "1"
    qint64 size;            // logical volume size (accurate for usage display)
    qint64 freeSpace;
    qint64 startingOffset = 0; // physical byte offset on disk (for proportional bar)
    qint64 physicalSize   = 0; // raw partition size from layout (for proportional bar)
    bool isLogical;         // True if it has a drive letter
};

// Disk health (SMART) summary for ATA/NVMe drives, including USB-bridged ones.
// Fields set to -1 mean "not reported by the device".
struct SmartHealthInfo {
    bool valid = false;         // true if SMART data was successfully read
    bool usbBus = false;        // device sits behind a USB bridge (so "no SMART" usually means the bridge/device can't expose it)
    QString protocol;           // "NVMe", "ATA", "ATA (USB/SAT)" or "NVMe (USB)"
    int temperatureC = -1;
    qint64 powerOnHours = -1;
    qint64 powerCycles = -1;
    int percentageUsed = -1;    // wear: NVMe "Percentage Used" or 100 - SSD life left
    int availableSpare = -1;    // NVMe remaining spare capacity %
    qint64 reallocatedSectors = -1;   // ATA attribute 05
    qint64 pendingSectors = -1;       // ATA attribute C5
    qint64 uncorrectableSectors = -1; // ATA attribute C6
    qint64 mediaErrors = -1;    // NVMe media/data integrity errors
    qint64 unsafeShutdowns = -1;
    double hostWrittenTb = -1;  // total host writes in TB
    int healthPercent = -1;     // 0-100 overall health score
    QString verdict;            // "Good", "Warning", "Critical" ("" when !valid)
    QStringList issues;         // reasons behind a Warning/Critical verdict
};

// Everything the New Volume Wizard collects (mirrors Windows' New Simple Volume Wizard).
struct NewVolumeOptions {
    enum Mount { AutoLetter, Letter, Folder, NoLetter };
    QString diskNum;
    bool initializeDisk = false;         // only for a disk with no partitions
    QString partitionStyle = "gpt";      // "gpt" | "mbr" (used when initializeDisk)
    qint64 sizeMB = 0;                   // 0 = use the whole largest free extent
    QString fileSystem = "ntfs";         // "ntfs" | "exfat" | "fat32"
    int unitBytes = 0;                   // allocation unit size, 0 = default
    bool quickFormat = true;
    QString label;
    Mount mount = AutoLetter;
    QString driveLetter;                 // single A-Z (Mount == Letter), else empty
    QString mountFolder;                 // Mount == Folder
};

// One physical disk with its partitions, SMART health and Windows Recovery
// Environment (WinRE) state; drives the Storage tab and the disk tools.
struct StorageDisk {
    QString deviceID;
    QString model;
    QString interfaceType;
    QString mediaType;
    QString diskNumber;
    qint64 size;
    qint64 unallocatedSpace;
    bool isEmpty;           // True if the disk has no partitions or all space is unallocated
    bool isSystem;          // True if this disk contains the system partition (C:)
    bool isOffline = false; // True if Windows has the whole disk offline (DISK_ATTRIBUTE_OFFLINE)
    QString winReStatus;    // "Enabled" / "Disabled" for the system disk (blank on others unless WinRE physically lives there)
    QString winReLocation;  // reagentc location (blank when unregistered)
    QString winRePartitionNumber;
    qint64 winRePartitionSize = 0;
    bool hasRecoveryPartition = false;
    bool winReImagePresent = false;
    bool winReImageKnown = false;
    bool recoveryTypeCorrect = false;
    QList<PartitionInfo> partitions;
    SmartHealthInfo smart;
};

// One battery section (title + ordered key/value rows) from powercfg's report.
struct BatteryInfo {
    QString title;
    QList<QPair<QString, QString>> data;
};

// Worker-side engine. Create it, moveToThread() a QThread, then invoke runScan().
class HardwareModel : public QObject {
    Q_OBJECT

public:
    explicit HardwareModel(QObject *parent = nullptr);
    ~HardwareModel();

    // Helper functions (OS name, connectivity check, app dir, file search, elevation)
    static QString detectOs();
    static bool checkInternetConnection();
    static QString getBaseDir();
    static QString findFile(const QString &baseDir, const QString &pattern);
    static bool isUserAdmin();

    // Actions (static, blocking: run off the GUI thread)
    // Disables Windows Fast Startup (hiberboot) so disks are fully released.
    static QString disableFastStartup();
    // If a previous shrink-to-fit clone was interrupted (crash / power loss) it
    // left the source C: shrunk and possibly hibernation off. Undoes both and
    // returns a log line describing what was done, or an empty string if there
    // was nothing to recover. Blocking (runs PowerShell) — call off the UI thread.
    static QString recoverInterruptedClone();
    static QPair<bool, QString> clearDisk(const QString &diskNum);
    // Brings a disk that Windows has marked Offline back Online (and clears the
    // read-only attribute) so its volumes mount and get drive letters.
    static QPair<bool, QString> setDiskOnline(const QString &diskNum);
    // Safely removes (ejects) a disk the way "Safely Remove Hardware" does:
    // dismounts its volumes and asks PnP to eject the topmost removable ancestor
    // of the physical drive, so an external/USB drive can be unplugged safely.
    // On failure the returned string is the reason (e.g. a device still in use).
    static QPair<bool, QString> ejectDisk(const QString &diskNum);
    using WinReProgress = std::function<void(int, const QString &)>;
    static QPair<bool, QString> extendVolume(const QString &driveLetter);
    // Absorbs tail unallocated space into the system volume (C:) when a recovery
    // partition sits between C: and that free space (so a plain `extend` can't
    // reach it). Runs entirely inside the running OS — no WinPE reboot: it moves
    // WinRE back onto C: (reagentc /disable), deletes the recovery partition,
    // online-extends C: leaving a tail reserve, rebuilds a fresh recovery
    // partition there, and re-enables WinRE. Reuses WinReProgress for staging.
    // Precondition (caller-enforced): every partition between C: and the tail
    // unallocated space is a recovery partition; a real data partition there
    // makes it refuse. C: is only extended (never shrunk), so its data is safe.
    // NOTE: this is deliberately system-disk-only. A generalized non-system-disk
    // version (mergeUnallocatedAcrossRecoveryOnDisk / extendPartition /
    // findLargestLetteredPartition / checkExtendBlocker) was tried and removed:
    // on an external/USB disk it corrupted the partition table badly enough to
    // make a cloned disk unbootable (0xc000000e). Re-add only with a much more
    // rigorous rebuild + verification story than diskpart extend+create gave us.
    static QPair<bool, QString> mergeUnallocatedAcrossRecovery(
        const WinReProgress &progress = {});
    // Creates one partition in the disk's free space and formats/mounts it
    // according to `opt` (the New Volume Wizard's choices). On success the
    // returned string is where it mounted ("E:", a folder path, or "" for no
    // letter); on failure it is the diagnostic output.
    static QPair<bool, QString> createVolume(const NewVolumeOptions &opt);

public slots:
    void runScan();
    // Re-reads ONLY the storage/disk details (used to auto-refresh on disk
    // insert/removal without re-scanning every hardware subsystem). Emits
    // storageReady with the fresh list.
    void runStorageScan();
    // Keep the original, recovery-preserving clone as its own entry point.
    void runSystemClone(int sourceIdx, int targetIdx);
    void runSystemCloneWithoutRecovery(int sourceIdx, int targetIdx);
    void runSystemCloneExpanded(int sourceIdx, int targetIdx);
    // Shrink-disk clone: large source → smaller target. Temporarily shrinks the
    // source system volume to fit (used data must fit), clones, then grows the
    // source back. Recovery partition is dropped on the target.
    void runSystemCloneShrink(int sourceIdx, int targetIdx);

public:
    // Thread-safe: called from the GUI thread while runSystemClone runs on the
    // worker thread. The copy loops abort at the next block boundary and
    // cloneFinished(false, "__CANCELLED__") is emitted.
    void requestCloneCancel() { m_cloneCancelRequested.store(true); }

signals:
    void progress(int current, int total, QString label);
    void finished(QString title, QString summary, QList<StorageDisk> storage, QMap<QString, QString> motherboard, QList<BatteryInfo> battery, SystemSummaryData overview);
    void error(QString msg);

    // Progressive results — emitted as soon as each piece is detected so the UI
    // can show cards incrementally (fast/important first, slow/minor last).
    void cardReady(QString stage, SystemSummaryData overview);
    void storageReady(QList<StorageDisk> storage);
    void motherboardReady(QMap<QString, QString> motherboard);
    void batteryReady(QList<BatteryInfo> battery);

    // Driver check runs entirely on its own and reports after the rest of the
    // scan has already been shown.
    void driverResult(QString text);

    // Clone signals
    void cloneProgress(int percent);
    void cloneProgressDetail(int percent, qint64 copiedBytes, qint64 totalBytes, double mibPerSec, qint64 etaSecs);
    // Coarse phase of the clone, as a translation key (clone_stage_*), so the
    // UI has something to show during phases that emit no byte progress
    // (diskpart prep, VSS snapshot, Resize-Partition).
    void cloneStatus(QString stageKey);
    void cloneFinished(bool success, QString msg);

private:
    // Shared implementation used by the three explicitly separated entry points.
    void runSystemCloneMode(int sourceIdx, int targetIdx, int cloneMode);
    QString getWindowsInfo(std::function<void(int, int, QString)> progressCallback, SystemSummaryData &overview);
    QList<StorageDisk> getStorageDetails();
    QMap<QString, QString> getMotherboardInfo();
    QList<BatteryInfo> getBatteryDetails();

    // WMI Helper
    bool initWmi();
    void cleanupWmi();
    QList<QMap<QString, QVariant>> queryWmi(const QString &query);

    // Kicks off powercfg /batteryreport in the background so it runs in parallel
    // with the (slower) WMI queries instead of adding ~1s serially.
    void startBatteryReportAsync();
    std::atomic<bool> m_cloneCancelRequested{false};

    QProcess *m_batteryReportProc = nullptr;
    QString m_batteryReportPath;

    // The slow "problem drivers" enumeration runs entirely on its own; its result
    // is collected AFTER the rest of the scan is emitted (see readDriverResult).
    void startDriverCheckAsync();
    QString readDriverResult();
    QProcess *m_driverProc = nullptr;
    QString m_driverPath;

    // Encryption status is a separate, fast background probe so it's ready in time
    // for the System card, without waiting on the slow driver check.
    void startEncryptionAsync();
    QProcess *m_encProc = nullptr;
    QString m_encryptionPath;

    // Internal state for WMI (raw COM pointers kept as void* to avoid pulling
    // Windows COM headers into this widely-included header)
    bool wmiInitialized;
    bool comInitialized; // Track if we successfully called CoInitialize
    void* m_pLoc; // IWbemLocator*
    void* m_pSvc; // IWbemServices*
};

#endif // HARDWAREMODEL_H
