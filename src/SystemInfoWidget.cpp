// =============================================================================
// SystemInfoWidget.cpp - card-based System Summary page.
// =============================================================================
#include "SystemInfoWidget.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QProgressBar>
#include <QRegularExpression>
#include <cmath>
#include <algorithm>

// ── i18n ─────────────────────────────────────────────────
// Current language for the labels this widget draws. Set at the start of
// updateData() before the cards are rebuilt.
// (module-level state, see comment above)
static QString g_lang = "en";
// Picks the string for the current language (g_lang): English, Chinese or French.
static QString L(const QString &en, const QString &zh, const QString &fr) {
    if (g_lang == "zh") return zh;
    if (g_lang == "fr") return fr;
    return en;
}

// ── palette ──────────────────────────────────────────────
static const char *PAGE_BG    = "#eef2f5";
static const char *CARD_BG    = "#ffffff";
static const char *CARD_HDR   = "#5C7C98";
static const char *CARD_BORD  = "#c8d8e8";
static const char *FG         = "#2a3a4a";
static const char *FG_TITLE   = "#ffffff";
static const char *FG_DIM     = "#6a7a8a";
static const char *FG_OK      = "#1a7a1a";
static const char *FG_WARN    = "#cc2222";
// ─────────────────────────────────────────────────────────

// ── card builder ─────────────────────────────────────────

// Returns the card QFrame and sets *bodyOut to the inner body layout.
// (see comment above) Builds an empty card: coloured title bar + body area.
static QFrame *startCard(const QString &title, QVBoxLayout **bodyOut)
{
    auto *card = new QFrame;
    card->setObjectName("Card");
    card->setStyleSheet(
        QString("QFrame#Card{"
                " background:%1;"
                " border:1px solid %2;"
                " border-radius:8px;"
                "}").arg(CARD_BG, CARD_BORD));

    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    auto *hdr = new QLabel(title);
    hdr->setObjectName("CardHdr");
    hdr->setStyleSheet(
        QString("QLabel#CardHdr{"
                " color:%1;"
                " background:%2;"
                " font-size:11px;"
                " font-weight:bold;"
                " font-family:Consolas;"
                " letter-spacing:1px;"
                " padding:8px 14px;"
                " border-bottom:1px solid %3;"
                " border-top-left-radius:7px;"
                " border-top-right-radius:7px;"
                "}").arg(FG_TITLE, CARD_HDR, CARD_BORD));
    cl->addWidget(hdr);

    auto *body = new QWidget;
    body->setStyleSheet("background:transparent;");
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(14, 12, 14, 14);
    bl->setSpacing(6);
    cl->addWidget(body);

    *bodyOut = bl;
    return card;
}

// ── label helpers ─────────────────────────────────────────

// Label helper: normal value text.
static QLabel *val(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(
        QString("color:%1; font-size:11px; font-family:Consolas;").arg(FG));
    l->setWordWrap(true);
    return l;
}

// Label helper: dimmed secondary text.
static QLabel *dim(const QString &text)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(
        QString("color:%1; font-size:11px; font-family:Consolas;").arg(FG_DIM));
    l->setWordWrap(true);
    return l;
}

// Label helper: monospace text in an explicit colour (status OK/warn).
static QLabel *colored(const QString &text, const char *color)
{
    auto *l = new QLabel(text);
    l->setStyleSheet(
        QString("color:%1; font-size:11px; font-family:Consolas;").arg(color));
    return l;
}

// ── battery parser ────────────────────────────────────────

// One battery as shown on a card, parsed from the raw powercfg report sections.
struct BatteryCard {
    QString label;
    QString name;
    QString manufacturer;
    QString charge;
    QString health;
    QString designCap;
    QString fullChargeCap;
    QString cycleCount;
};

// Converts BatteryInfo sections into BatteryCards; sets isDesktop when no
// battery exists (desktop PC).
static QList<BatteryCard> parseBatteries(const QList<BatteryInfo> &battery,
                                          bool &isDesktop)
{
    isDesktop = false;
    QList<BatteryCard> cards;
    int wmiIdx = 0;

    for (const auto &info : battery) {
        // Desktop / no battery
        if (info.title.contains("System Type", Qt::CaseInsensitive) ||
            info.title.contains("System Power", Qt::CaseInsensitive)) {
            for (const auto &kv : info.data)
                if (kv.second.contains("Desktop", Qt::CaseInsensitive))
                    isDesktop = true;
            continue;
        }

        // WMI physical battery
        if (info.title.contains("Battery (WMI)", Qt::CaseInsensitive)) {
            BatteryCard bc;
            bc.label = QString("%1 %2").arg(L("Battery", "电池", "Batterie")).arg(++wmiIdx);
            for (const auto &kv : info.data) {
                if      (kv.first == "Name")             bc.name   = kv.second;
                else if (kv.first == "Estimated Charge") bc.charge = kv.second;
            }
            cards.append(bc);
            continue;
        }

        // powercfg health data — attach to matching WMI card by index (#1, #2, ...)
        if (info.title.contains("powercfg", Qt::CaseInsensitive)) {
            // Extract 1-based index from title "Battery Health (powercfg) #N"
            int targetIdx = 0; // default: first card
            QRegularExpression idxRe("#(\\d+)");
            auto m = idxRe.match(info.title);
            if (m.hasMatch())
                targetIdx = m.captured(1).toInt() - 1;

            while (cards.size() <= targetIdx)
                cards.append(BatteryCard{});

            auto &bc = cards[targetIdx];
            for (const auto &kv : info.data) {
                if      (kv.first == "Health")               bc.health        = kv.second;
                else if (kv.first == "Design Capacity")      bc.designCap     = kv.second;
                else if (kv.first == "Full Charge Capacity") bc.fullChargeCap = kv.second;
                else if (kv.first == "Cycle Count")          bc.cycleCount    = kv.second;
                else if (kv.first == "Manufacturer")         bc.manufacturer  = kv.second;
            }
        }
    }
    return cards;
}

// ── card builders ─────────────────────────────────────────

// Card builders: each turns part of SystemSummaryData into one titled card.
// System: model, OS, activation, encryption.
static QFrame *buildSystemCard(const SystemSummaryData &ov,
                                const QMap<QString, QString> &mb,
                                bool isDesktopPower)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("SYSTEM", "系统", "SYSTEME"), &bl);

    // Model | OS — two balanced columns (no stray pipe)
    auto *row = new QWidget;
    row->setStyleSheet("background:transparent;");
    auto *rl = new QHBoxLayout(row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(20);

    if (!ov.systemModel.isEmpty()) {
        auto *w = val(ov.systemModel);
        w->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        rl->addWidget(w, 1);
    }
    if (!ov.osName.isEmpty()) {
        auto *w = dim(ov.osName);
        w->setAlignment(Qt::AlignTop | Qt::AlignRight);
        rl->addWidget(w, 1);
    }
    bl->addWidget(row);

    // Activation row
    auto *actRow = new QWidget;
    actRow->setStyleSheet("background:transparent;");
    auto *al = new QHBoxLayout(actRow);
    al->setContentsMargins(0, 0, 0, 0);
    al->setSpacing(24);

    const QString actYes = "✔ " + L("Activated", "已激活", "Active");
    const QString actNo  = "✘ " + L("Not Activated", "未激活", "Non active");
    if (!ov.winActivation.isEmpty()) {
        bool ok = (ov.winActivation.compare("Activated", Qt::CaseInsensitive) == 0);
        al->addWidget(colored(QString("Windows: %1").arg(ok ? actYes : actNo), ok ? FG_OK : FG_WARN));
    }
    if (!ov.officeActivation.isEmpty()) {
        bool ok = (ov.officeActivation.compare("Activated", Qt::CaseInsensitive) == 0);
        al->addWidget(colored(QString("Office: %1").arg(ok ? actYes : actNo), ok ? FG_OK : FG_WARN));
    }
    al->addStretch();
    bl->addWidget(actRow);

    QString serial = mb.value("Motherboard Serial Number");
    if (!serial.isEmpty()
        && serial != "To Be Filled By O.E.M."
        && serial.toLower() != "default string") {
        bl->addWidget(dim("S/N: " + serial));
    }

    if (isDesktopPower)
        bl->addWidget(dim(L("Power: Desktop — AC (no battery)",
                            "电源: 台式机 — 交流供电 (无电池)",
                            "Alimentation : ordinateur de bureau — secteur (sans batterie)")));

    return card;
}

// Windows / Office activation status card.
static QFrame *buildActivationCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("ACTIVATION & SECURITY", "激活与安全", "ACTIVATION & SECURITE"), &bl);

    bool any = false;
    const QString actYes = "✔ " + L("Activated", "已激活", "Active");
    const QString actNo  = "✘ " + L("Not Activated", "未激活", "Non active");

    if (!ov.winActivation.isEmpty()) {
        bool ok = (ov.winActivation.compare("Activated", Qt::CaseInsensitive) == 0);
        bl->addWidget(colored(QString("Windows: %1").arg(ok ? actYes : actNo), ok ? FG_OK : FG_WARN));
        any = true;
    }
    if (!ov.officeActivation.isEmpty()) {
        bool ok = (ov.officeActivation.compare("Activated", Qt::CaseInsensitive) == 0);
        bl->addWidget(colored(QString("Office: %1").arg(ok ? actYes : actNo), ok ? FG_OK : FG_WARN));
        any = true;
    }
    if (!any) bl->addWidget(dim("—"));
    return card;
}

// CPU name, cores and threads.
static QFrame *buildCpuCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard("CPU", &bl);

    if (ov.cpuName.isEmpty()) {
        bl->addWidget(dim("—"));
    } else {
        bl->addWidget(val(ov.cpuName));
        if (ov.cpuCores > 0 || ov.cpuThreads > 0) {
            QString line;
            if (ov.cpuCores   > 0) line += QString("%1: %2").arg(L("Cores", "核心", "Coeurs")).arg(ov.cpuCores);
            if (ov.cpuThreads > 0) {
                if (!line.isEmpty()) line += "    ";
                line += QString("%1: %2").arg(L("Threads", "线程", "Threads")).arg(ov.cpuThreads);
            }
            bl->addWidget(dim(line));
        }
    }
    for (const QString &npu : ov.npus) {
        bl->addSpacing(6);
        bl->addWidget(dim("NPU"));
        bl->addWidget(val(npu));
    }
    return card;
}

// GPU(s) with VRAM, and NPU(s) if present.
static QFrame *buildGpuCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard("GPU", &bl);

    if (ov.gpus.isEmpty()) {
        bl->addWidget(dim("—"));
    } else {
        for (const auto &gpu : ov.gpus) {
            bl->addWidget(val(gpu.name));
            if (gpu.vramMb > 0) {
                double gb = gpu.vramMb / 1024.0;
                QString s = gb >= 1.0
                    ? QString("VRAM: %1 GB").arg(gb, 0, 'f', 0)
                    : QString("VRAM: %1 MB").arg(gpu.vramMb);
                bl->addWidget(dim(s));
            }
            if (&gpu != &ov.gpus.last()) bl->addSpacing(6);
        }
    }
    return card;
}

// Total RAM, type/speed and per-slot module details.
static QFrame *buildMemoryCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("MEMORY", "内存", "MEMOIRE"), &bl);

    if (ov.totalRamBytes <= 0) {
        bl->addWidget(dim("—"));
        return card;
    }

    const double GiB = 1024.0 * 1024 * 1024;
    double gb = ov.totalRamBytes / GiB;
    bl->addWidget(val(QString("%1 GB").arg(qRound(gb))));

    // Slot usage: populated / total.
    if (ov.ramSlotsTotal > 0 || !ov.ramSticks.isEmpty()) {
        int used  = ov.ramSticks.isEmpty() ? ov.ramStickSizes.size() : ov.ramSticks.size();
        int total = ov.ramSlotsTotal > 0 ? ov.ramSlotsTotal : used;
        bl->addWidget(dim(QString("%1: %2 / %3")
            .arg(L("Slots used", "已用插槽", "Emplacements utilises"))
            .arg(used).arg(total)));
    }

    if (!ov.ramSticks.isEmpty()) {
        bl->addSpacing(2);
        for (const auto &rs : ov.ramSticks) {
            QString line;
            if (!rs.slot.isEmpty()) line += rs.slot + ":  ";
            line += QString("%1 GB").arg(rs.sizeBytes / GiB, 0, 'f', 0);
            if (!rs.type.isEmpty())      line += "  " + rs.type;
            if (rs.speedMhz > 0)         line += QString("  %1 MHz").arg(rs.speedMhz);
            if (!rs.manufacturer.isEmpty()) line += "  ·  " + rs.manufacturer;
            bl->addWidget(dim(line));
        }
    } else {
        // Fallback when per-stick WMI data is unavailable.
        QString typeSpeed;
        if (!ov.ramType.isEmpty())   typeSpeed += ov.ramType;
        if (ov.ramSpeedMhz > 0)      typeSpeed += QString("  %1 MHz").arg(ov.ramSpeedMhz);
        if (!typeSpeed.isEmpty())    bl->addWidget(dim(typeSpeed));
    }

    return card;
}

// Resolution / refresh rate of each display and touch capability.
static QFrame *buildDisplayCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("DISPLAY", "显示器", "AFFICHAGE"), &bl);

    // Still running — this card fills in once the display step completes.
    if (!ov.displayReady) {
        bl->addWidget(dim(L("Reading display information...",
                            "正在读取显示信息...",
                            "Lecture des informations d'affichage...")));
        return card;
    }

    if (ov.displays.isEmpty()) {
        bl->addWidget(dim("—"));
    } else {
        for (const auto &d : ov.displays) {
            QString line = QString("%1 × %2").arg(d.width).arg(d.height);
            if (d.refreshRate > 0)
                line += QString("  |  %1 Hz").arg(d.refreshRate);
            bl->addWidget(val(line));
        }
    }

    // Touchscreen indicator.
    const QString touchTxt = ov.hasTouch
        ? "✔ " + L("Touchscreen", "触摸屏", "Ecran tactile")
        : L("Touchscreen", "触摸屏", "Ecran tactile") + ": " + L("No", "无", "Non");
    bl->addWidget(colored(touchTxt, ov.hasTouch ? FG_OK : FG_DIM));

    return card;
}

// Physical network adapters with MAC addresses.
static QFrame *buildNetworkCard(const SystemSummaryData &ov)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("NETWORK", "网络", "RESEAU"), &bl);

    if (ov.networkAdapters.isEmpty()) {
        bl->addWidget(dim("—"));
        return card;
    }

    for (const auto &nic : ov.networkAdapters) {
        const QString kind = nic.wireless ? L("Wi-Fi", "无线", "Wi-Fi")
                                           : L("Ethernet", "有线", "Ethernet");
        bl->addWidget(val(nic.name));
        QString sub = kind;
        if (!nic.mac.isEmpty()) sub += "    " + nic.mac;
        bl->addWidget(dim(sub));
        if (&nic != &ov.networkAdapters.last()) bl->addSpacing(6);
    }
    return card;
}

// Battery name, manufacturer, charge, health, capacity and cycle count.
static QFrame *buildBatteryCard(const BatteryCard &bc)
{
    QVBoxLayout *bl;
    auto *card = startCard(bc.label.toUpper(), &bl);

    if (!bc.name.isEmpty())         bl->addWidget(val(bc.name));
    if (!bc.manufacturer.isEmpty()) bl->addWidget(dim(L("Manufacturer", "制造商", "Fabricant") + ": " + bc.manufacturer));
    if (!bc.name.isEmpty() || !bc.manufacturer.isEmpty()) bl->addSpacing(4);

    if (!bc.health.isEmpty()) {
        // Extract numeric value to pick color
        bool ok;
        double pct = bc.health.left(bc.health.indexOf('%')).toDouble(&ok);
        const char *col = (!ok || pct >= 80) ? FG_OK : (pct >= 50 ? FG : FG_WARN);
        auto *hl = colored(L("Health", "健康度", "Sante") + ": " + bc.health, col);
        hl->setStyleSheet(hl->styleSheet() + " font-weight:bold;");
        bl->addWidget(hl);
    }
    if (!bc.charge.isEmpty())       bl->addWidget(dim(L("Charge", "电量", "Charge") + ": " + bc.charge));
    if (!bc.designCap.isEmpty())    bl->addWidget(dim(L("Design", "设计容量", "Conception") + ": " + bc.designCap));
    if (!bc.fullChargeCap.isEmpty())bl->addWidget(dim(L("Full Charge", "满充容量", "Charge max") + ": " + bc.fullChargeCap));
    if (!bc.cycleCount.isEmpty())   bl->addWidget(dim(L("Cycles", "循环次数", "Cycles") + ": " + bc.cycleCount));

    if (bc.health.isEmpty() && bc.charge.isEmpty()
        && bc.designCap.isEmpty() && bc.fullChargeCap.isEmpty())
        bl->addWidget(dim(L("No data available", "无可用数据", "Aucune donnee disponible")));

    return card;
}

// ── Windows Disk Management–style layout helpers ───────────

// Colours and sizes imitating Windows Disk Management's graphical view.
static const char *DM_PANEL_BG  = "#f0f0f0";
static const char *DM_ROW_BORDER = "#a0a0a0";
static const char *DM_CHART_BG  = "#e8e8e8";
static const char *DM_HEADER    = "#1a2d4d";
static const char *DM_PART_BORDER = "#7a7a7a";
static const int   DM_META_WIDTH = 188;
static const int   DM_CHART_HEIGHT = 112;
static const int   DM_HEADER_H = 12;

// Size text in Disk Management style (MB / GB / TB).
static QString formatSizeDiskMgmt(qint64 bytes)
{
    const double gb = bytes / (1024.0 * 1024 * 1024);
    if (gb >= 1.0)
        return QString("%1 GB").arg(gb, 0, 'f', 2);
    const double mb = bytes / (1024.0 * 1024);
    return QString("%1 MB").arg(mb, 0, 'f', 0);
}

// True for the EFI System Partition (small FAT32/unlabelled system partition).
static bool isEfiStylePartition(const PartitionInfo &p)
{
    const QString fs = p.fileSystem.toUpper();
    if (p.physicalSize <= 0 || p.physicalSize >= 600LL * 1024 * 1024)
        return false;
    return fs.contains("FAT") || fs.contains("EFI");
}

// True for a Windows recovery partition (no drive letter, recovery label/size).
static bool isRecoveryStylePartition(const PartitionInfo &p)
{
    const QString v = p.volumeName.toLower();
    return v.contains("recover") || v.contains("winre");
}

// Third line of a partition cell, e.g. "Healthy (Primary Partition)".
static QString partitionHealthLine(const PartitionInfo &p)
{
    const QString healthy = L("Healthy", "状态良好", "Sain");
    if (isEfiStylePartition(p))
        return healthy + L(" (EFI System Partition)", " (EFI 系统分区)", " (Partition systeme EFI)");
    if (isRecoveryStylePartition(p))
        return healthy + L(" (Recovery Partition)", " (恢复分区)", " (Partition de recuperation)");
    if (p.isLogical && p.deviceID.startsWith("C:", Qt::CaseInsensitive))
        return healthy + L(" (Boot, Page File, Crash Dump, Primary Partition)",
                           " (启动, 页面文件, 故障转储, 主分区)",
                           " (Demarrage, fichier d'echange, vidage, partition principale)");
    if (p.isLogical)
        return healthy + L(" (Basic Data Partition)", " (基本数据分区)", " (Partition de donnees de base)");
    return healthy + L(" (Primary Partition)", " (主分区)", " (Partition principale)");
}

// First line of a partition cell: label and drive letter.
static QString partitionTitleLine(const PartitionInfo &p)
{
    if (p.isLogical) {
        const QString letter = p.deviceID;
        if (!p.volumeName.isEmpty()
            && p.volumeName.compare("Local Disk", Qt::CaseInsensitive) != 0)
            return QString("%1 (%2)").arg(p.volumeName, letter);
        return "(" + letter + ")";
    }
    if (!p.volumeName.isEmpty())
        return p.volumeName;
    return QString();
}

// Second line of a partition cell: size and file system.
static QString partitionSizeFsLine(const PartitionInfo &p, qint64 physBytes)
{
    QString fs = p.fileSystem;
    if (fs.isEmpty() || fs == "Unknown")
        fs.clear();
    return formatSizeDiskMgmt(physBytes) + (fs.isEmpty() ? QString() : (" " + fs));
}

// Collect partitions with positive size, disk order (already sorted by offset in model).
// (see comment above)
static QList<const PartitionInfo *> orderedPartitions(const StorageDisk &disk)
{
    QList<const PartitionInfo *> out;
    for (const auto &pp : disk.partitions) {
        const qint64 psz = (pp.physicalSize > 0) ? pp.physicalSize : pp.size;
        if (psz > 0)
            out.append(&pp);
    }
    return out;
}

// One coloured block in the disk bar (colour by EFI / recovery / data / free).
static QWidget *buildPartitionCell(const PartitionInfo &part, const StorageDisk &disk, qint64 physBytes)
{
    auto *outer = new QFrame;
    outer->setFrameShape(QFrame::StyledPanel);
    outer->setStyleSheet(
        QString("QFrame{border:1px solid %1; background:#ffffff; border-radius:0px;}")
        .arg(DM_PART_BORDER));
    outer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    outer->setMinimumWidth(28);

    auto *vl = new QVBoxLayout(outer);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);

    auto *head = new QLabel;
    head->setFixedHeight(DM_HEADER_H);
    head->setStyleSheet(QString("background:%1; border:none;").arg(DM_HEADER));

    auto *body = new QWidget;
    const bool bootVol = part.isLogical
        && part.deviceID.startsWith("C:", Qt::CaseInsensitive);
    body->setStyleSheet(bootVol
        ? "background-color:#ececec;"
        : "background-color:#ffffff;");

    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(4, 4, 4, 4);
    bl->setSpacing(2);

    QString l1 = partitionTitleLine(part);
    QString l2 = partitionSizeFsLine(part, physBytes);
    QString l3 = partitionHealthLine(part);
    if (l1.isEmpty() && isEfiStylePartition(part))
        l1 = "EFI";

    auto addLine = [&](const QString &s, bool bold) {
        if (s.isEmpty())
            return;
        auto *lb = new QLabel(s);
        lb->setWordWrap(true);
        lb->setAlignment(Qt::AlignHCenter);
        lb->setStyleSheet(
            QString("color:%1; font-size:8px; font-family:Segoe UI;%2")
            .arg(FG, bold ? " font-weight:bold;" : ""));
        bl->addWidget(lb);
    };

    addLine(l1, true);
    addLine(l2, false);
    addLine(l3, false);

    // Usage line for logical volumes with valid size info
    if (part.isLogical && part.size > 0 && part.freeSpace >= 0) {
        const qint64 used = part.size - part.freeSpace;
        const double usedRatio = qBound(0.0, static_cast<double>(used) / part.size, 1.0);
        const bool highUsage = usedRatio >= 0.9;
        const QString barColor = highUsage ? "#cc2222" : "#3a7fc1";

        const QString usageTxt = formatSizeDiskMgmt(used) + " " + L("used", "已用", "utilise") + " / "
                                 + formatSizeDiskMgmt(part.freeSpace) + " " + L("free", "可用", "libre");
        addLine(usageTxt, false);

        // Mini usage bar using stretch-based layout
        auto *barBg = new QWidget;
        barBg->setFixedHeight(4);
        barBg->setStyleSheet("background:#d0d8e0; border-radius:2px;");
        auto *barLayout = new QHBoxLayout(barBg);
        barLayout->setContentsMargins(0, 0, 0, 0);
        barLayout->setSpacing(0);

        auto *fill = new QWidget;
        fill->setFixedHeight(4);
        fill->setStyleSheet(QString("background:%1; border-radius:2px;").arg(barColor));
        barLayout->addWidget(fill, qRound(usedRatio * 1000));

        auto *gap = new QWidget;
        gap->setFixedHeight(4);
        gap->setStyleSheet("background:transparent;");
        barLayout->addWidget(gap, qRound((1.0 - usedRatio) * 1000));

        bl->addWidget(barBg);
    }

    vl->addWidget(head);
    vl->addWidget(body, 1);

    QString tip = l1;
    if (!tip.isEmpty())
        tip += "\n";
    tip += l2 + "\n" + l3;
    tip += QString("\n\nOffset: %1 · Partition size: %2")
        .arg(QString::number(part.startingOffset))
        .arg(QString::number(physBytes));
    outer->setToolTip(tip);

    return outer;
}

// Proportional row (Qt stretch) — widths track disk size like Disk Management, without paint rounding drift.
// (see comment above)
static QWidget *buildDiskLayoutChart(const StorageDisk &disk)
{
    auto *wrap = new QWidget;
    wrap->setStyleSheet(QString("background:%1;").arg(DM_CHART_BG));
    wrap->setMinimumHeight(DM_CHART_HEIGHT);
    wrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *hl = new QHBoxLayout(wrap);
    hl->setContentsMargins(1, 1, 1, 1);
    hl->setSpacing(0);

    const qint64 total = disk.size;
    const QList<const PartitionInfo *> parts = orderedPartitions(disk);

    if (total <= 0 || parts.isEmpty()) {
        auto *empty = new QLabel(parts.isEmpty() ? "No partitions" : "—");
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet("color:#666; font-size:9px;");
        hl->addWidget(empty, 1);
        return wrap;
    }

    const int SCALE = 100000;
    const double invTotal = 1.0 / static_cast<double>(total);

    for (const PartitionInfo *pp : parts) {
        const qint64 psz = (pp->physicalSize > 0) ? pp->physicalSize : pp->size;
        const int st = qMax(1, static_cast<int>(std::llround(static_cast<double>(psz) * invTotal
                                                             * static_cast<double>(SCALE))));
        hl->addWidget(buildPartitionCell(*pp, disk, psz), st);
    }

    if (disk.unallocatedSpace > 0) {
        const int ust = qMax(1, static_cast<int>(std::llround(
            static_cast<double>(disk.unallocatedSpace) * invTotal * static_cast<double>(SCALE))));
        auto *unalloc = new QLabel(L("Unallocated", "未分配", "Non alloue"));
        unalloc->setAlignment(Qt::AlignCenter);
        unalloc->setWordWrap(true);
        unalloc->setStyleSheet(
            QString("background:%1; color:%2; font-size:8px; font-family:Segoe UI; border:none;")
            .arg("#dde4eb", FG_DIM));
        hl->addWidget(unalloc, ust);
    }

    return wrap;
}

// One disk row: left metadata (like Disk Management) + chart.
// (see comment above)
static QWidget *buildDiskManagementRow(const StorageDisk &disk)
{
    auto *row = new QFrame;
    row->setStyleSheet(
        QString("QFrame#DMRow{background:#ffffff; border:1px solid %1; border-radius:0px;}")
        .arg(DM_ROW_BORDER));
    row->setObjectName("DMRow");

    auto *hl = new QHBoxLayout(row);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);

    auto *meta = new QFrame;
    meta->setFixedWidth(DM_META_WIDTH);
    meta->setStyleSheet(
        QString("background:%1; border:none; border-right:1px solid %2;")
        .arg(DM_PANEL_BG, DM_ROW_BORDER));

    auto *mv = new QVBoxLayout(meta);
    mv->setContentsMargins(10, 10, 10, 10);
    mv->setSpacing(4);

    auto *lDisk = new QLabel(QString("Disk %1").arg(disk.deviceID));
    lDisk->setStyleSheet(QString("color:%1; font-size:11px; font-weight:bold; font-family:Segoe UI;").arg(FG));

    auto *lModel = new QLabel(disk.model.isEmpty() ? "—" : disk.model);
    lModel->setWordWrap(true);
    lModel->setStyleSheet(QString("color:%1; font-size:9px; font-family:Segoe UI;").arg(FG_DIM));

    auto *lBasic = new QLabel("Basic");
    lBasic->setStyleSheet(QString("color:%1; font-size:10px; font-family:Segoe UI;").arg(FG));

    auto *lSize = new QLabel(formatSizeDiskMgmt(disk.size));
    lSize->setStyleSheet(QString("color:%1; font-size:10px; font-family:Segoe UI;").arg(FG));

    auto *lOnline = new QLabel(disk.isEmpty ? "Empty" : "Online");
    lOnline->setStyleSheet(QString("color:%1; font-size:10px; font-family:Segoe UI;").arg(FG));

    mv->addWidget(lDisk);
    mv->addWidget(lModel);
    mv->addWidget(lBasic);
    mv->addWidget(lSize);
    mv->addWidget(lOnline);

    if (disk.smart.valid) {
        QString verdictText;
        const char *color = FG_OK;
        if (disk.smart.verdict == "Good") {
            verdictText = L("Good", "良好", "Bon");
        } else if (disk.smart.verdict == "Warning") {
            verdictText = L("Warning", "警告", "Attention");
            color = "#cc7a00";
        } else {
            verdictText = L("Critical", "严重", "Critique");
            color = FG_WARN;
        }

        QString line = L("Health: ", "健康: ", "Sante: ") + verdictText;
        if (disk.smart.healthPercent >= 0)
            line += QString(" %1%").arg(disk.smart.healthPercent);
        if (disk.smart.temperatureC >= 0)
            line += QString(" · %1°C").arg(disk.smart.temperatureC);
        if (disk.smart.percentageUsed >= 0)
            line += QString(" · %1 %2%").arg(L("Wear", "磨损", "Usure")).arg(disk.smart.percentageUsed);

        auto *lHealth = new QLabel(line);
        lHealth->setWordWrap(true);
        lHealth->setStyleSheet(QString("color:%1; font-size:10px; font-weight:bold; font-family:Segoe UI;").arg(color));

        QStringList tip;
        if (disk.smart.powerOnHours >= 0)
            tip << L("Power-on hours: ", "通电时间: ", "Heures sous tension: ") + QString::number(disk.smart.powerOnHours) + " h";
        if (disk.smart.powerCycles >= 0)
            tip << L("Power cycles: ", "通电次数: ", "Cycles d'alimentation: ") + QString::number(disk.smart.powerCycles);
        if (disk.smart.hostWrittenTb >= 0)
            tip << L("Total writes: ", "累计写入: ", "Ecritures totales: ") + QString::number(disk.smart.hostWrittenTb, 'f', 1) + " TB";
        if (!disk.smart.issues.isEmpty())
            tip << disk.smart.issues.join("\n");
        if (!tip.isEmpty())
            lHealth->setToolTip(tip.join("\n"));

        mv->addWidget(lHealth);
    }

    if (disk.unallocatedSpace > 0) {
        auto *lUn = new QLabel(QString("%1: %2").arg(L("Unallocated", "未分配", "Non alloue")).arg(formatSizeDiskMgmt(disk.unallocatedSpace)));
        lUn->setStyleSheet(QString("color:%1; font-size:9px; font-family:Segoe UI;").arg(FG_DIM));
        mv->addWidget(lUn);
    }

    mv->addStretch();

    hl->addWidget(meta);
    hl->addWidget(buildDiskLayoutChart(disk), 1);

    return row;
}

// ── storage card ──────────────────────────────────────────

// All disks, each as a Disk Management-style row.
static QFrame *buildStorageCard(const QList<StorageDisk> &storage)
{
    QVBoxLayout *bl;
    auto *card = startCard(L("STORAGE", "存储", "STOCKAGE"), &bl);

    if (storage.isEmpty()) {
        bl->addWidget(dim(L("No drives detected", "未检测到驱动器", "Aucun disque detecte")));
        return card;
    }

    bl->addWidget(dim(L("Layout matches Disk Management proportions — use Storage Details for actions.",
                        "布局比例与磁盘管理一致 — 操作请使用“存储详情”。",
                        "La disposition respecte les proportions de Gestion des disques — utilisez Details stockage pour les actions.")));

    bool firstDisk = true;
    for (const auto &disk : storage) {
        if (!firstDisk)
            bl->addSpacing(10);
        firstDisk = false;
        bl->addWidget(buildDiskManagementRow(disk));
    }

    return card;
}

// ── widget ────────────────────────────────────────────────

// Sets up the scroll area and its style sheet.
SystemInfoWidget::SystemInfoWidget(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(
        QString("QScrollArea{border:none; background:%1;}"
                "QScrollBar:vertical{background:%1; width:8px; border-radius:4px;}"
                "QScrollBar::handle:vertical{background:#b0c4d4; border-radius:4px;}"
                "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}")
        .arg(PAGE_BG));
}

// Replaces the page with a centred placeholder text (e.g. "Scanning...").
void SystemInfoWidget::showLoading(const QString &message)
{
    if (QWidget *old = takeWidget()) old->deleteLater();
    m_grid = nullptr; // grid belonged to the old widget; addCard must not touch it

    auto *root = new QWidget;
    root->setObjectName("SIRoot");
    root->setStyleSheet(QString("QWidget#SIRoot{background:%1;}").arg(PAGE_BG));

    auto *vl = new QVBoxLayout(root);
    vl->addStretch();
    auto *lb = new QLabel(message);
    lb->setAlignment(Qt::AlignCenter);
    lb->setStyleSheet(QString("color:%1; font-size:15px;").arg(FG_DIM));
    vl->addWidget(lb);
    vl->addStretch();

    setWidget(root);
    setWidgetResizable(true);
}

// Full rebuild: discards the old page and creates every card in a fixed order.
void SystemInfoWidget::updateData(const SystemSummaryData &overview,
                                   const QList<StorageDisk> &storage,
                                   const QList<BatteryInfo> &battery,
                                   const QMap<QString, QString> &motherboard,
                                   const QString &lang)
{
    g_lang = lang;
    if (QWidget *old = takeWidget()) old->deleteLater();
    m_grid = nullptr; // grid belonged to the old widget; addCard must not touch it

    auto *root = new QWidget;
    root->setObjectName("SIRoot");
    root->setStyleSheet(
        QString("QWidget#SIRoot{background:%1;}").arg(PAGE_BG));

    auto *vl = new QVBoxLayout(root);
    vl->setContentsMargins(16, 14, 16, 16);
    vl->setSpacing(10);

    bool isDesktop = false;
    QList<BatteryCard> batCards = parseBatteries(battery, isDesktop);

    vl->addWidget(buildSystemCard(overview, motherboard, isDesktop));

    {
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        row->addWidget(buildCpuCard(overview), 1, Qt::AlignTop);
        row->addWidget(buildGpuCard(overview), 1, Qt::AlignTop);
        vl->addLayout(row);
    }

    bool displayShown = false;
    {
        auto *mid = new QHBoxLayout;
        mid->setSpacing(12);
        mid->addWidget(buildMemoryCard(overview), 1, Qt::AlignTop);

        if (isDesktop) {
            mid->addWidget(buildDisplayCard(overview), 1, Qt::AlignTop);
            displayShown = true;
        } else if (!batCards.isEmpty()) {
            if (batCards.size() == 1) {
                mid->addWidget(buildBatteryCard(batCards.first()), 1, Qt::AlignTop);
            } else {
                auto *wrap = new QWidget;
                auto *vb = new QVBoxLayout(wrap);
                vb->setContentsMargins(0, 0, 0, 0);
                vb->setSpacing(12);
                for (const auto &bc : batCards)
                    vb->addWidget(buildBatteryCard(bc));
                mid->addWidget(wrap, 1, Qt::AlignTop);
            }
        } else {
            mid->addWidget(buildDisplayCard(overview), 1, Qt::AlignTop);
            displayShown = true;
        }
        vl->addLayout(mid);
    }

    // On laptops the Display card gets bumped by the Battery card above; show it
    // on its own row so resolution / touchscreen info is still visible.
    if (!displayShown) {
        auto *row = new QHBoxLayout;
        row->setSpacing(12);
        row->addWidget(buildDisplayCard(overview), 1, Qt::AlignTop);
        if (!overview.networkAdapters.isEmpty())
            row->addWidget(buildNetworkCard(overview), 1, Qt::AlignTop);
        else
            row->addStretch(1);
        vl->addLayout(row);
    } else if (!overview.networkAdapters.isEmpty()) {
        vl->addWidget(buildNetworkCard(overview));
    }

    vl->addWidget(buildStorageCard(storage));

    vl->addStretch();
    setWidget(root);
}

// ── progressive rendering ─────────────────────────────────

// Starts an empty page with a 2-column grid that addCard() etc. fill in.
void SystemInfoWidget::beginProgressive(const QString &lang)
{
    g_lang = lang;
    if (QWidget *old = takeWidget()) old->deleteLater();

    m_gridRow = 0;
    m_gridCol = 0;

    auto *root = new QWidget;
    root->setObjectName("SIRoot");
    root->setStyleSheet(QString("QWidget#SIRoot{background:%1;}").arg(PAGE_BG));

    auto *outer = new QVBoxLayout(root);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(10);

    m_grid = new QGridLayout();
    m_grid->setSpacing(12);
    m_grid->setColumnStretch(0, 1);
    m_grid->setColumnStretch(1, 1);
    outer->addLayout(m_grid);
    outer->addStretch();

    setWidget(root);
    setWidgetResizable(true);
}

// (see header)
void SystemInfoWidget::placeCard(QFrame *card, bool fullWidth)
{
    if (!m_grid) return;
    if (fullWidth) {
        if (m_gridCol != 0) { m_gridRow++; m_gridCol = 0; }
        m_grid->addWidget(card, m_gridRow, 0, 1, 2, Qt::AlignTop);
        m_gridRow++;
        m_gridCol = 0;
    } else {
        m_grid->addWidget(card, m_gridRow, m_gridCol, Qt::AlignTop);
        if (++m_gridCol >= 2) { m_gridCol = 0; m_gridRow++; }
    }
}

// Adds the card matching a scan stage name (cpu/gpu/memory/system/display/
// network/activation); unknown stages are ignored.
void SystemInfoWidget::addCard(const QString &stage, const SystemSummaryData &ov)
{
    if (!m_grid) return;
    if (stage == "cpu")             placeCard(buildCpuCard(ov), false);
    else if (stage == "gpu")        placeCard(buildGpuCard(ov), false);
    else if (stage == "memory")     placeCard(buildMemoryCard(ov), false);
    else if (stage == "system")     placeCard(buildSystemCard(ov, {}, false), true);
    else if (stage == "display")    placeCard(buildDisplayCard(ov), false);
    else if (stage == "network") {
        if (!ov.networkAdapters.isEmpty()) placeCard(buildNetworkCard(ov), true);
    }
    else if (stage == "activation") placeCard(buildActivationCard(ov), false);
}

// Adds the full-width storage card.
void SystemInfoWidget::addStorageCard(const QList<StorageDisk> &storage)
{
    if (!m_grid) return;
    placeCard(buildStorageCard(storage), true);
}

// Adds one card per battery found in the report.
void SystemInfoWidget::addBatteryCards(const QList<BatteryInfo> &battery)
{
    if (!m_grid) return;
    bool isDesktop = false;
    const auto cards = parseBatteries(battery, isDesktop);
    for (const auto &bc : cards)
        placeCard(buildBatteryCard(bc), false);
}
