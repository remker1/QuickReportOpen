// Disk-operation handlers for MainWindow, split out of MainWindow.cpp.
// Covers: storage context menu, clear / initialize / online / extend volume,
// WinRE restore, storage-only refresh, and the system-clone flows.
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
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QScrollBar>
#include <memory>
#include <algorithm>
#include <thread>
#include "CrashHandler.h"
#include "CloneProgressDialog.h"
#include "NewVolumeWizard.h"
#include "Translations.h"
#include "hardwaretester/TesterTranslations.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

// Human-readable byte size, used by the clone size reporting below.
// (binary units: B, KiB, MiB, GiB, TiB; two decimals)
static QString formatBytes(qint64 bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QString("%1 %2").arg(v, 0, 'f', 2).arg(units[u]);
}

// Forward declaration (defined near the bottom of the file).
static qint64 largestFreeExtent(const StorageDisk &d);

// Right-click menu of the Storage tree. Only top-level disk rows get a menu.
// Entries depend on the disk: Set Online (offline disks), Clear Disk (never the
// system disk), Extend System Volume (system disk with free space), New Volume,
// Clone to this disk, and Safely Remove (USB disks only).
void MainWindow::onStorageContextMenu(const QPoint &pos) {
    QTreeWidgetItem *item = storageTree->itemAt(pos);
    if (!item) return;

    // Check if it's a top-level disk item (has UserRole data)
    QVariant diskData = item->data(0, Qt::UserRole);
    QVariant partData = item->data(1, Qt::UserRole); // Partition number stored in column 1 UserRole

    QMenu menu(this);

    if (diskData.isValid() && !partData.isValid()) {
        // It's a Disk
        QString diskNum = diskData.toString();
        bool isEmptyDisk = item->data(0, Qt::UserRole + 2).toBool();

        // Restrict destructive actions on the actual system disk (where Windows / C:
        // lives), not on a hardcoded "Disk 0" which is not always the system disk.
        bool isSystemDisk = (!systemDiskNum.isEmpty() && diskNum == systemDiskNum);

        // Bring an Offline disk back Online — non-destructive, so it leads the menu.
        bool isOffline = item->data(0, Qt::UserRole + 4).toBool();
        if (isOffline && !isSystemDisk) {
            QAction *onlineAction = menu.addAction(getTranslation("context_set_online"));
            connect(onlineAction, &QAction::triggered, [this, diskNum]() {
                onSetDiskOnline(diskNum);
            });
            menu.addSeparator();
        }

        // Safely remove (eject) — only for USB drives, so an external stick/SSD
        // can be pulled quickly. Internal (SATA/NVMe) disks are never offered
        // eject. USB is detected from the SMART bus probe (authoritative: the
        // drive sits behind a USB bridge) with WMI InterfaceType as a fallback.
        // It is non-destructive: PnP vetoes it if the drive is still in use.
        bool isUsbDisk = false;
        for (const auto &d : m_scanStorage) {
            if (d.diskNumber == diskNum) {
                isUsbDisk = d.smart.usbBus ||
                            d.interfaceType.compare("USB", Qt::CaseInsensitive) == 0;
                break;
            }
        }
        // Never present Diskpart Clean for the Windows disk. Keep the hard
        // guard in onClearDisk as a second layer of protection for indirect calls.
        if (!isSystemDisk) {
            QAction *clearAction = menu.addAction(getTranslation("context_clear_disk"));
            connect(clearAction, &QAction::triggered, [this, diskNum]() {
                QString title = getTranslation("confirm_clear_title");
                QString msg = getTranslation("confirm_clear_msg").replace("{disk_num}", diskNum);
                if (QMessageBox::question(this, title, msg, QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
                    onClearDisk(diskNum);
                }
            });
        }

        // One-click: extend the system volume into adjacent unallocated space.
        if (isSystemDisk) {
            qulonglong unalloc = item->data(0, Qt::UserRole + 3).toULongLong();
            if (unalloc > 0) {
                QAction *extendAction = menu.addAction(getTranslation("context_extend_system"));
                connect(extendAction, &QAction::triggered, this, &MainWindow::onExtendSystemVolume);
            }
        }

        // New Volume wizard: any disk with a big enough free extent (an empty
        // disk is initialized by the wizard itself).
        {
            qint64 freeBytes = 0;
            for (const auto &d : m_scanStorage) {
                if (d.diskNumber == diskNum) {
                    freeBytes = d.partitions.isEmpty() ? d.size : largestFreeExtent(d);
                    break;
                }
            }
            if (freeBytes >= 64LL * 1024 * 1024 && !isOffline) {
                QAction *newVolAction = menu.addAction(getTranslation("context_new_volume"));
                connect(newVolAction, &QAction::triggered, [this, diskNum]() {
                    onNewVolume(diskNum);
                });
            }
        }

        // Clone Action
        // Only allow cloning TO this disk if it's NOT the system disk. The three
        // clone flavours (fast mirror, expand, shrink) are merged behind one
        // entry: onSystemCloneAuto() picks the right one from the source/target
        // sizes and WinRE. The individual runSystemClone* modes are kept.
        if (!isSystemDisk) {
            QAction *cloneAction = menu.addAction(getTranslation("context_clone_disk"));
            connect(cloneAction, &QAction::triggered, [this, diskNum]() {
                onSystemCloneAuto(diskNum);
            });
        }

        if (!isSystemDisk && isUsbDisk) {
            menu.addSeparator();
            QAction *ejectAction = menu.addAction(getTranslation("context_eject_disk"));
            connect(ejectAction, &QAction::triggered, [this, diskNum]() {
                onEjectDisk(diskNum);
            });
        }
    } else {
        return;
    }

    menu.exec(storageTree->viewport()->mapToGlobal(pos));
}

// Single "Clone" entry. Picks the clone mode from the source/target sizes:
//   0 = 1:1 mirror keeping recovery, 1 = mirror without recovery,
//   2 = expand system volume to fill a bigger target, 3 = shrink to fit a smaller
//   target. Refuses early if the used data cannot fit on the target.
void MainWindow::onSystemCloneAuto(const QString &targetDiskNum) {
    // Single "Clone" entry point: work out which of the clone flavours fits and
    // hand off to onSystemCloneDisk(). Nothing here is destructive — that
    // routine still shows its own confirmation before touching a disk.
    //
    // Mode picking, from the physical disk sizes and the source's real usage:
    //   target clearly larger than source  → 2 (expand system volume to fill)
    //   target smaller than source         → 3 (shrink: copy used data only)
    //   target ~= source                   → 0/1 (1:1 mirror; recovery if WinRE)
    // Priorities are stability and speed, so the picker is deliberately
    // conservative: it prefers the fast 1:1 mirror whenever the target can hold
    // the source layout, only picks the slower shrink path when the target is
    // genuinely smaller, and refuses up front (before the confirmation and
    // before any VSS/shrink work) when even the source's used data cannot fit.

    // ── resolve source (system) and target from the last scan ──────────────
    // Trust the reliably-inferred system disk number first; the WMI isSystem
    // flag is only a fallback because it is occasionally set on the wrong disk
    // or not at all. Never let a stray isSystem flag override an explicit match.
    const StorageDisk *source = nullptr;
    const StorageDisk *target = nullptr;
    const StorageDisk *isSystemFallback = nullptr;
    for (const auto &d : m_scanStorage) {
        if (!systemDiskNum.isEmpty() && d.diskNumber == systemDiskNum)
            source = &d;
        else if (d.isSystem)
            isSystemFallback = &d;
        if (d.diskNumber == targetDiskNum)
            target = &d;
    }
    if (!source)
        source = isSystemFallback;

    // If either disk is missing from the scan, or they somehow resolve to the
    // same disk, don't guess a resizing mode. Hand off with the safe 1:1 mirror;
    // onSystemCloneDisk re-resolves the system disk itself and still confirms.
    if (!source || !target || source->diskNumber == target->diskNumber) {
        onSystemCloneDisk(targetDiskNum, 0);
        return;
    }

    const qint64 sourceSize = source->size;
    const qint64 targetSize = target->size;
    const bool sourceHasWinRe = (source->winReStatus == "Enabled");

    // Real floor for a shrink clone: every partition on the source (EFI, MSR,
    // recovery, C:, …) is copied, so the target must hold the sum of used data
    // across all of them — not just C:. Logical volumes report an accurate
    // freeSpace (QStorageInfo); non-logical partitions report freeSpace == 0 and
    // are counted whole, which is correct because they are copied in full.
    qint64 sourceUsed = 0;
    bool sawSystemFree = false;   // did we get a trustworthy free-space reading on C:?
    QString sysDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
    if (sysDrive.size() >= 2 && sysDrive[1] == ':')
        sysDrive = sysDrive.left(2).toUpper();
    for (const auto &p : source->partitions) {
        const qint64 used = (p.freeSpace > 0 && p.freeSpace <= p.size)
                                ? (p.size - p.freeSpace)
                                : p.size;
        if (used > 0)
            sourceUsed += used;
        if (p.isLogical && p.freeSpace > 0 &&
            p.deviceID.startsWith(sysDrive, Qt::CaseInsensitive))
            sawSystemFree = true;
    }

    int mode;
    const qint64 tolerance = 1LL * 1024 * 1024 * 1024; // 1 GiB
    // Only bother expanding when the target is meaningfully larger; otherwise a
    // straight 1:1 mirror is faster and avoids the recovery-partition relocation
    // that the expand path performs. A few GiB of slack also keeps same-class
    // drives that differ only by vendor rounding on the mirror path.
    const qint64 expandThreshold = qMax<qint64>(4LL * 1024 * 1024 * 1024,
                                                sourceSize / 20); // ≥4 GiB or 5%

    if (sourceSize <= 0 || targetSize <= 0) {
        // Sizes unknown — fall back to the safe 1:1 mirror; onSystemCloneDisk
        // still validates the target and confirms with the user.
        mode = sourceHasWinRe ? 0 : 1;
    } else if (targetSize + tolerance < sourceSize) {
        // Target is physically smaller: only a shrink-to-fit clone can work.
        // Refuse now if even the used data (plus the worker's own headroom)
        // cannot fit, so we never run VSS + shrink only to fail midway. Guard
        // the refusal behind a trustworthy free-space reading to avoid a false
        // rejection of a target that would actually fit.
        const qint64 headroom = qMax<qint64>(2LL * 1024 * 1024 * 1024, sourceUsed / 10);
        if (sawSystemFree && sourceUsed > 0 && sourceUsed + headroom > targetSize) {
            const QString msg = getTranslation("clone_target_too_small_msg")
                                    .replace("{target}", targetDiskNum)
                                    .replace("{used}", QString::number(sourceUsed / 1e9, 'f', 1))
                                    .replace("{need}", QString::number((sourceUsed + headroom) / 1e9, 'f', 1))
                                    .replace("{have}", QString::number(targetSize / 1e9, 'f', 1));
            QMessageBox::warning(this, getTranslation("clone_target_too_small_title"), msg);
            return;
        }
        mode = 3; // target smaller → shrink
    } else if (targetSize > sourceSize + expandThreshold) {
        mode = 2; // target clearly larger → expand to fill
    } else {
        mode = sourceHasWinRe ? 0 : 1; // ~same size → 1:1 mirror
    }

    onSystemCloneDisk(targetDiskNum, mode);
}

// Runs the chosen clone mode: resolves the system (source) disk, asks for
// confirmation, creates the progress dialog and a worker thread + HardwareModel,
// wires progress/cancel/finish signals, keeps the PC awake while cloning, and on
// completion reports success / cancel / error and optionally shuts the PC down.
void MainWindow::onSystemCloneDisk(const QString &targetDiskNum, int cloneMode) {
    // Same system-disk resolution as the other clone entry points.
    if (systemDiskNum.isEmpty()) {
        QString systemDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
        if (systemDrive.size() >= 2 && systemDrive[1] == ':') {
            systemDrive = systemDrive.left(2).toUpper();
        }

        for (int i = 0; i < storageTree->topLevelItemCount() && systemDiskNum.isEmpty(); ++i) {
            QTreeWidgetItem *diskItem = storageTree->topLevelItem(i);
            if (!diskItem) continue;

            QString diskNum = diskItem->data(0, Qt::UserRole).toString();
            if (diskNum.isEmpty()) continue;

            std::function<bool(QTreeWidgetItem*)> hasSystemPartition = [&](QTreeWidgetItem *node) -> bool {
                if (!node) return false;
                if (node->text(0).startsWith(systemDrive, Qt::CaseInsensitive)) return true;
                for (int c = 0; c < node->childCount(); ++c) {
                    if (hasSystemPartition(node->child(c))) return true;
                }
                return false;
            };

            if (hasSystemPartition(diskItem)) {
                systemDiskNum = diskNum;
            }
        }
    }

    if (systemDiskNum.isEmpty()) {
        QMessageBox::critical(this, getTranslation("error_title"), "Could not identify system disk.");
        return;
    }

    if (targetDiskNum == systemDiskNum) {
        QMessageBox::warning(this, getTranslation("error_title"), "Cannot clone to the system disk.");
        return;
    }

    const QString confirmTitleKey = cloneMode == 0 ? "confirm_system_clone_title"
                                  : cloneMode == 1 ? "confirm_system_clone_no_recovery_title"
                                  : cloneMode == 3 ? "confirm_system_clone_shrink_title"
                                                   : "confirm_system_clone_expand_recovery_title";
    const QString confirmMessageKey = cloneMode == 0 ? "confirm_system_clone_msg"
                                    : cloneMode == 1 ? "confirm_system_clone_no_recovery_msg"
                                    : cloneMode == 3 ? "confirm_system_clone_shrink_msg"
                                                     : "confirm_system_clone_expand_recovery_msg";
    QString title = getTranslation(confirmTitleKey);
    QString msg = getTranslation(confirmMessageKey)
                      .replace("{target}", targetDiskNum).replace("{source}", systemDiskNum);
    if (QMessageBox::question(this, title, msg, QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    const int sourceDisk = systemDiskNum.toInt();
    const int targetDisk = targetDiskNum.toInt();

    auto *progressDialog = new CloneProgressDialog(
        [this](const QString &k) { return getTranslation(k); }, this);
    progressDialog->setWindowTitle(getTranslation("clone_dialog_title"));
    auto diskDetail = [this](int num) -> QString {
        for (const auto &d : m_scanStorage) {
            if (d.diskNumber.toInt() == num) {
                const QString model = d.model.trimmed();
                const QString size = formatBytes(d.size);
                return model.isEmpty() ? size : model + "  ·  " + size;
            }
        }
        return QString();
    };
    progressDialog->setDisks(
        getTranslation("clone_progress_source").replace("{disk}", QString::number(sourceDisk)),
        diskDetail(sourceDisk),
        getTranslation("clone_progress_target").replace("{disk}", QString::number(targetDisk)),
        diskDetail(targetDisk));
    progressDialog->setStage(getTranslation("clone_stage_prepare"));

    QThread *cloneThread = new QThread;
    HardwareModel *cloneModel = new HardwareModel;
    cloneModel->moveToThread(cloneThread);

    // Cancel: flag the worker (thread-safe atomic); it aborts at the next
    // 64 MiB block and reports back through cloneFinished. The dialog shows
    // its own "cancelling" state and stays open until then.
    connect(progressDialog, &CloneProgressDialog::canceled, this, [cloneModel]() {
        cloneModel->requestCloneCancel();
    });

    connect(cloneThread, &QThread::started, cloneModel,
            [cloneModel, sourceDisk, targetDisk, cloneMode]() {
        if (cloneMode == 0)
            cloneModel->runSystemClone(sourceDisk, targetDisk);
        else if (cloneMode == 1)
            cloneModel->runSystemCloneWithoutRecovery(sourceDisk, targetDisk);
        else if (cloneMode == 3)
            cloneModel->runSystemCloneShrink(sourceDisk, targetDisk);
        else
            cloneModel->runSystemCloneExpanded(sourceDisk, targetDisk);
    });

    // Worker objects must be destroyed in their own thread. Directly deleting
    // them from the GUI callback caused a crash whenever cloning failed quickly.
    connect(cloneModel, &HardwareModel::cloneFinished,
            cloneModel, &QObject::deleteLater);
    connect(cloneModel, &HardwareModel::cloneFinished,
            cloneThread, &QThread::quit);
    connect(cloneThread, &QThread::finished,
            cloneThread, &QObject::deleteLater);

    connect(cloneModel, &HardwareModel::cloneProgress, progressDialog, &CloneProgressDialog::setValue);
    connect(cloneModel, &HardwareModel::cloneProgressDetail, progressDialog, &CloneProgressDialog::setDetail);
    connect(cloneModel, &HardwareModel::cloneStatus, progressDialog,
            [this, progressDialog](const QString &stageKey) {
        progressDialog->setStage(getTranslation(stageKey));
    });

    connect(cloneModel, &HardwareModel::cloneFinished, this, [this, progressDialog, cloneMode](bool success, QString msg) {
        #ifdef Q_OS_WIN
        // Clone done — let the machine sleep / turn the screen off again.
        SetThreadExecutionState(ES_CONTINUOUS);
        #endif
        // Read at the last possible moment so the option can be changed at any
        // time before the clone finishes. It is intentionally ignored on
        // cancellation and failure.
        const bool shutdownAfterSuccess = success && progressDialog->shutdownRequested();
        // Programmatic close must not emit canceled() and touch a worker which
        // has already finished. Defer widget destruction to the GUI event loop.
        progressDialog->blockSignals(true);
        progressDialog->close();
        progressDialog->deleteLater();

        if (success) {
            const QString successKey = cloneMode == 0 ? "system_clone_success_msg"
                                     : cloneMode == 1 ? "system_clone_no_recovery_success_msg"
                                     : cloneMode == 3 ? "system_clone_shrink_success_msg"
                                                      : "system_clone_expand_recovery_success_msg";
            const QString successText = getTranslation(successKey).replace("{stats}", msg);
            if (shutdownAfterSuccess) {
                const bool scheduled = QProcess::startDetached(
                    "shutdown.exe",
                    QStringList() << "/s" << "/t" << "10" << "/c"
                                  << getTranslation("clone_shutdown_comment"));
                if (scheduled) {
                    statusBar->showMessage(getTranslation("clone_shutdown_scheduled"));
                } else {
                    QMessageBox::critical(this, getTranslation("error_title"),
                                          getTranslation("clone_shutdown_failed"));
                    QMessageBox::information(this, getTranslation("clone_success_title"),
                                             successText);
                    refreshStorageOnly();
                }
            } else {
                QMessageBox::information(this, getTranslation("clone_success_title"),
                                         successText);
                refreshStorageOnly();
            }
        } else if (msg == "__CANCELLED__") {
            QMessageBox::information(this, getTranslation("confirm_system_clone_title"),
                                     getTranslation("clone_cancelled_msg"));
            refreshStorageOnly();
        } else {
            QMessageBox::critical(this, getTranslation("error_title"), getTranslation("clone_failed_msg").replace("{error}", msg));
        }
    });

    progressDialog->show();
    #ifdef Q_OS_WIN
    // Keep the system awake and the display on for the whole clone.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    #endif
    cloneThread->start();
}

// Runs `work` on a background thread under a modal busy dialog (see header).
QPair<bool, QString> MainWindow::runDiskOpWithBusy(
    const QString &message, std::function<QPair<bool, QString>()> work) {
    // Run the blocking diskpart/format work on a background thread while showing
    // a modal, indeterminate busy dialog. The modal keeps the UI painted (no
    // "not responding") and blocks other input, so the operation can't overlap
    // another. A local event loop waits for the worker to post its completion.
    QProgressDialog dlg(message, QString(), 0, 0, this); // min==max==0 → busy bar
    dlg.setWindowTitle(getTranslation("please_wait"));
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setCancelButton(nullptr);       // a diskpart/format op must run to completion
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);

    QEventLoop loop;
    QPair<bool, QString> result;
    std::thread worker([&]() {
        result = work();
        // Post back to the GUI thread's event loop to end the wait.
        QMetaObject::invokeMethod(&loop, "quit", Qt::QueuedConnection);
    });
    dlg.show();
    loop.exec();
    worker.join();
    dlg.reset();
    return result;
}

// Wipes a disk (diskpart clean) after a hard guard against the system disk.
void MainWindow::onClearDisk(const QString &diskNum) {
    // Hard guard: never clear the system disk, regardless of how we got here.
    if (!systemDiskNum.isEmpty() && diskNum == systemDiskNum) {
        QMessageBox::warning(this, getTranslation("context_clear_disk"),
                             getTranslation("system_disk_protected"));
        return;
    }

    QPair<bool, QString> result = runDiskOpWithBusy(
        getTranslation("disk_op_running"),
        [diskNum]() { return HardwareModel::clearDisk(diskNum); });
    if (result.first) {
        QMessageBox::information(this, getTranslation("context_clear_disk"), getTranslation("disk_cleared_success").replace("{disk_num}", diskNum));
        refreshStorageOnly();
    } else {
        QMessageBox::critical(this, getTranslation("error_title"), getTranslation("disk_clear_failed").replace("{disk_num}", diskNum).replace("{error}", result.second));
    }
}

// Brings an offline disk online and clears its read-only flag.
void MainWindow::onSetDiskOnline(const QString &diskNum) {
    QPair<bool, QString> result = runDiskOpWithBusy(
        getTranslation("disk_op_running"),
        [diskNum]() { return HardwareModel::setDiskOnline(diskNum); });
    if (result.first) {
        QMessageBox::information(this, getTranslation("context_set_online"),
                                 getTranslation("disk_online_success").replace("{disk_num}", diskNum));
        refreshStorageOnly();
    } else {
        QMessageBox::critical(this, getTranslation("error_title"),
                              getTranslation("disk_online_failed").replace("{disk_num}", diskNum).replace("{error}", result.second));
    }
}

// Safely removes a USB disk (like the tray "Eject").
void MainWindow::onEjectDisk(const QString &diskNum) {
    // Safely remove (eject) the disk. Non-destructive: PnP vetoes it if the drive
    // is fixed or still in use, in which case the reason is surfaced.
    QPair<bool, QString> result = runDiskOpWithBusy(
        getTranslation("disk_op_running"),
        [diskNum]() { return HardwareModel::ejectDisk(diskNum); });
    if (result.first) {
        QMessageBox::information(this, getTranslation("context_eject_disk"),
                                 getTranslation("disk_eject_success").replace("{disk_num}", diskNum));
        refreshStorageOnly();
    } else {
        QMessageBox::critical(this, getTranslation("error_title"),
                              getTranslation("disk_eject_failed").replace("{disk_num}", diskNum).replace("{error}", result.second));
    }
}

// Re-reads only the disks on a short-lived worker thread (falls back to a full
// scan when nothing has been scanned yet). Only one refresh runs at a time.
void MainWindow::refreshStorageOnly() {
    // If nothing has been scanned yet, do the first full scan; otherwise just
    // re-read the disks on its own lightweight worker thread.
    if (!m_hasScan) {
        onRefreshClicked();
        return;
    }
    if (m_storageRefreshing)
        return; // one already in flight; the debounce timer coalesces bursts
    m_storageRefreshing = true;

    QThread *thread = new QThread;
    HardwareModel *model = new HardwareModel;
    model->moveToThread(thread);
    connect(thread, &QThread::started, model, &HardwareModel::runStorageScan);
    connect(model, &HardwareModel::storageReady, this, &MainWindow::onStorageRefreshed);
    // Tear down on either success or error.
    connect(model, &HardwareModel::storageReady, thread, &QThread::quit);
    connect(model, &HardwareModel::error, thread, &QThread::quit);
    connect(thread, &QThread::finished, model, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this]() { m_storageRefreshing = false; });
    thread->start();
}

// Applies a fresh disk list to the Storage tree and the summary storage card.
void MainWindow::onStorageRefreshed(const QList<StorageDisk> &storage) {
    m_scanStorage = storage;
    populateStorageTree(storage);
    // Re-render the System Summary from cached data (no hardware re-scan) so its
    // storage card reflects the change without appending a duplicate card.
    if (m_hasScan)
        systemInfoTab->updateData(m_scanOverview, m_scanStorage, m_scanBattery,
                                  m_scanMotherboard, currentLang);
}

// Extends C: into adjacent unallocated space. Looks at the partition layout:
//   - lettered data partition in the way -> refuse
//   - recovery partition in the way      -> onMergeAcrossRecovery()
//   - free space right behind C:         -> plain extend, falling back to the
//     cross-recovery flow if the plain extend fails.
void MainWindow::onExtendSystemVolume() {
    QString sysDrive = qEnvironmentVariable("SystemDrive", "C:").trimmed();
    if (sysDrive.size() >= 2) sysDrive = sysDrive.left(2).toUpper();

    // ── Decide which path fits the layout (mirrors onSystemCloneAuto's style) ──
    // A plain diskpart `extend` only reaches unallocated space that is directly
    // after C:. When a recovery partition sits between C: and the tail free
    // space, we need the cross-recovery flow instead. If a real (lettered) data
    // partition sits in between, we refuse — moving its blocks is out of scope.
    const StorageDisk *sys = nullptr;
    for (const auto &d : m_scanStorage) {
        if (!systemDiskNum.isEmpty() && d.diskNumber == systemDiskNum) { sys = &d; break; }
        if (d.isSystem && !sys) sys = &d;
    }

    bool recoveryBlocks = false; // recovery partition(s) between C: and tail free
    bool dataBlocks = false;     // a lettered data partition blocks the way
    if (sys) {
        const PartitionInfo *cPart = nullptr;
        for (const auto &p : sys->partitions)
            if (p.isLogical && p.deviceID.startsWith(sysDrive, Qt::CaseInsensitive)) { cPart = &p; break; }
        if (cPart) {
            // Everything that starts at or after C:'s end sits between C: and the
            // tail free space — including a recovery partition that begins exactly
            // where C: ends (the common layout). Only C: itself and anything
            // before it are skipped.
            const qint64 cEnd = cPart->startingOffset +
                                (cPart->physicalSize > 0 ? cPart->physicalSize : cPart->size);
            // Free space right behind C: (a real gap before the next partition)
            // means a plain extend already works, whatever follows the gap.
            qint64 nextStart = -1;
            for (const auto &p : sys->partitions) {
                if (&p == cPart || p.startingOffset <= cPart->startingOffset)
                    continue;
                if (nextStart < 0 || p.startingOffset < nextStart)
                    nextStart = p.startingOffset;
            }
            const bool gapRightAfterC = nextStart < 0 || nextStart - cEnd >= 64LL * 1024 * 1024;
            for (const auto &p : sys->partitions) {
                if (gapRightAfterC)
                    break;
                if (&p == cPart || p.startingOffset <= cPart->startingOffset)
                    continue;
                if (p.isLogical)
                    dataBlocks = true;        // has a drive letter → user data
                else
                    recoveryBlocks = true;    // no letter → treat as recovery-like
            }
        }
    }

    if (dataBlocks) {
        QMessageBox::warning(this, getTranslation("context_extend_system"),
                             getTranslation("extend_blocked_data_msg"));
        return;
    }
    if (recoveryBlocks) {
        onMergeAcrossRecovery();
        return;
    }

    // ── Simple case: unallocated space is directly after C: ──
    QString title = getTranslation("context_extend_system");
    QString msg = getTranslation("confirm_extend_system_msg").replace("{drive}", sysDrive);
    if (QMessageBox::question(this, title, msg, QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    QPair<bool, QString> result = runDiskOpWithBusy(
        getTranslation("disk_op_running"),
        [sysDrive]() { return HardwareModel::extendVolume(sysDrive); });
    if (result.first) {
        QMessageBox::information(this, title, getTranslation("extend_success"));
        refreshStorageOnly();
    } else {
        // A plain extend only reaches free space directly behind C:. If it could
        // not (a recovery partition the layout scan missed, say), fall back to
        // the cross-recovery flow, which re-checks the layout itself and asks for
        // its own confirmation, rather than dead-ending on a diskpart message.
        CrashHandler::log("Plain extend of the system volume failed, trying the cross-recovery flow: "
                          + result.second.left(300));
        QMessageBox::information(this, title, getTranslation("extend_fallback_msg"));
        onMergeAcrossRecovery();
    }
}

// Heavy multi-step flow (delete recovery, extend C:, rebuild recovery, re-enable
// WinRE) on a worker thread with a localized, non-cancellable progress dialog
// that turns red once the critical stage starts (do not power off).
void MainWindow::onMergeAcrossRecovery() {
    // Heavy, multi-step operation: deletes the recovery partition, online-extends
    // C:, rebuilds a recovery partition at the tail, and re-enables WinRE. Confirm
    // with a message distinct from the plain-extend one, then run it on a worker
    // thread behind a non-cancellable progress dialog — the core routine runs to
    // completion once the critical stage begins, so there is no cancel button
    // to manage.
    if (QMessageBox::warning(this, getTranslation("merge_recovery_title"),
                             getTranslation("confirm_merge_recovery_msg"),
                             QMessageBox::Yes | QMessageBox::No,
                             QMessageBox::No) != QMessageBox::Yes)
        return;

    auto *dialog = new QProgressDialog(this);
    dialog->setWindowTitle(getTranslation("merge_recovery_title"));
    dialog->setRange(0, 100);
    dialog->setValue(0);
    dialog->setCancelButton(nullptr);      // runs to completion; no safe cancel
    dialog->setMinimumDuration(0);
    dialog->setAutoClose(false);
    dialog->setAutoReset(false);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->setWindowFlag(Qt::WindowCloseButtonHint, false);
    dialog->show();

    const QPointer<MainWindow> self(this);
    const QPointer<QProgressDialog> guardedDialog(dialog);
    const auto criticalStarted = std::make_shared<std::atomic_bool>(false);
    const auto operationFinished = std::make_shared<std::atomic_bool>(false);

    QThread *thread = QThread::create(
        [self, guardedDialog, criticalStarted, operationFinished]() {
        auto progress = [self, guardedDialog, criticalStarted, operationFinished](int percent, const QString &stage) {
            if (!self) return;
            if (stage == "critical")
                criticalStarted->store(true);
            QMetaObject::invokeMethod(self,
                [self, guardedDialog, criticalStarted, operationFinished, percent, stage]() {
                if (!self || !guardedDialog || operationFinished->load())
                    return;
                QString text;
                if (self->currentLang == "zh") {
                    static const QMap<QString, QString> labels{
                        {"checking", QStringLiteral("正在检查磁盘布局…")},
                        {"disabling_winre", QStringLiteral("正在将 Windows RE 退回系统盘…")},
                        {"deleting_recovery", QStringLiteral("正在删除原恢复分区…")},
                        {"extending", QStringLiteral("正在扩展系统卷…")},
                        {"rebuilding_recovery", QStringLiteral("正在磁盘末尾重建恢复分区…")},
                        {"enabling", QStringLiteral("正在重新启用 Windows RE…")},
                        {"verifying", QStringLiteral("正在验证…")},
                        {"critical", QStringLiteral("警告：已进入关键阶段，正在调整分区。请勿关机或断电！")},
                        {"complete", QStringLiteral("完成。")}};
                    text = labels.value(stage, stage);
                } else if (self->currentLang == "fr") {
                    static const QMap<QString, QString> labels{
                        {"checking", "Vérification de la disposition du disque…"},
                        {"disabling_winre", "Déplacement de Windows RE vers le disque système…"},
                        {"deleting_recovery", "Suppression de l'ancienne partition de récupération…"},
                        {"extending", "Extension du volume système…"},
                        {"rebuilding_recovery", "Reconstruction de la partition de récupération…"},
                        {"enabling", "Réactivation de Windows RE…"},
                        {"verifying", "Vérification…"},
                        {"critical", "AVERTISSEMENT : phase critique. N'éteignez pas le PC !"},
                        {"complete", "Terminé."}};
                    text = labels.value(stage, stage);
                } else {
                    static const QMap<QString, QString> labels{
                        {"checking", "Checking the disk layout…"},
                        {"disabling_winre", "Moving Windows RE back onto the system drive…"},
                        {"deleting_recovery", "Deleting the old recovery partition…"},
                        {"extending", "Extending the system volume…"},
                        {"rebuilding_recovery", "Rebuilding the recovery partition at the end of the disk…"},
                        {"enabling", "Re-enabling Windows RE…"},
                        {"verifying", "Verifying…"},
                        {"critical", "WARNING: critical stage. Do not power off the PC!"},
                        {"complete", "Done."}};
                    text = labels.value(stage, stage);
                }
                if (criticalStarted->load() && stage != "critical") {
                    const QString warn = self->currentLang == "zh"
                        ? QStringLiteral("警告：关键操作进行中，请勿关机或断电！")
                        : self->currentLang == "fr"
                            ? QStringLiteral("AVERTISSEMENT : opération critique en cours. N'éteignez pas le PC !")
                            : QStringLiteral("WARNING: critical operation in progress. Do not power off the PC!");
                    text = warn + "\n" + text;
                }
                guardedDialog->setLabelText(text);
                guardedDialog->setValue(percent);
                if (criticalStarted->load())
                    guardedDialog->setStyleSheet(
                        "QProgressDialog QLabel{color:#b42318;font-weight:700;}");
            }, Qt::QueuedConnection);
        };

        const QPair<bool, QString> result =
            HardwareModel::mergeUnallocatedAcrossRecovery(progress);
        operationFinished->store(true);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, guardedDialog, result]() {
            if (!self) return;
            if (guardedDialog) {
                guardedDialog->blockSignals(true);
                guardedDialog->hide();
                guardedDialog->deleteLater();
            }
            if (result.first) {
                QMessageBox::information(self, self->getTranslation("merge_recovery_title"),
                    self->getTranslation("merge_recovery_success").replace("{stats}", result.second));
                self->refreshStorageOnly();
            } else if (result.second == "__DATA_PARTITION__") {
                QMessageBox::warning(self, self->getTranslation("context_extend_system"),
                                     self->getTranslation("extend_blocked_data_msg"));
            } else {
                QMessageBox::critical(self, self->getTranslation("error_title"),
                    self->getTranslation("merge_recovery_failed").replace("{error}", result.second));
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// Largest single free extent on a disk, from its partition layout. A new
// partition can only live in one gap, so this (not the summed unallocated
// space) bounds the wizard's size choice.
static qint64 largestFreeExtent(const StorageDisk &d) {
    QList<QPair<qint64, qint64>> used; // [start, end)
    for (const auto &p : d.partitions) {
        const qint64 len = (p.physicalSize > 0) ? p.physicalSize : p.size;
        if (len > 0)
            used.append({p.startingOffset, p.startingOffset + len});
    }
    std::sort(used.begin(), used.end());
    qint64 best = 0, cursor = 0;
    for (const auto &u : used) {
        best = qMax(best, u.first - cursor);
        cursor = qMax(cursor, u.second);
    }
    return qMax(best, d.size - cursor);
}

// Opens the New Volume wizard for a disk and, if accepted, creates/formats the
// volume in the background. A disk without partitions is initialized by the wizard.
void MainWindow::onNewVolume(const QString &diskNum) {
    const StorageDisk *disk = nullptr;
    for (const auto &d : m_scanStorage) {
        if (d.diskNumber == diskNum) { disk = &d; break; }
    }
    if (!disk) return;

    // A disk with no partitions is initialized by the wizard; a disk that
    // already has partitions is never converted or cleaned — the new volume just
    // goes into its free space.
    const bool needsInit = disk->partitions.isEmpty();
    const qint64 freeBytes = needsInit ? disk->size : largestFreeExtent(*disk);
    if (freeBytes < 64LL * 1024 * 1024) return;

    NewVolumeWizard wizard(diskNum, disk->model, needsInit, freeBytes,
                           [this](const QString &k) { return getTranslation(k); }, this);
    const int accepted = wizard.exec();

    if (wizard.fallbackRequested()) {
        QProcess::startDetached(QStringLiteral("mmc.exe"), {QStringLiteral("diskmgmt.msc")});
        return;
    }
    if (accepted != QDialog::Accepted)
        return;

    const NewVolumeOptions opt = wizard.options();
    QPair<bool, QString> result = runDiskOpWithBusy(
        getTranslation("disk_op_running"),
        [opt]() { return HardwareModel::createVolume(opt); });
    if (result.first) {
        QMessageBox::information(this, getTranslation("context_new_volume"),
            result.second.isEmpty()
                ? getTranslation("nvw_done_none")
                : getTranslation("nvw_done").replace("{location}", result.second));
        refreshStorageOnly();
    } else {
        QMessageBox::critical(this, getTranslation("error_title"),
            getTranslation("nvw_failed").replace("{disk_num}", diskNum)
                .replace("{error}", result.second));
    }
}
