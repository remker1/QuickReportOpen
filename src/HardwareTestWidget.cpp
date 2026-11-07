// =============================================================================
// HardwareTestWidget.cpp - LEGACY tile launcher for individual hardware tests.
//
// Not part of the current CMake target; replaced by hardwaretester/HardwareTesterWidget.
// Kept for reference: contains stand-alone dialogs for dead-pixel, disk speed, USB
// port, CPU/RAM stress, input, camera, sound settings and touch tests.
// =============================================================================
#include "HardwareTestWidget.h"
#include "TouchTestWidget.h"
#include "InputTestWidget.h"

#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QDialog>
#include <QComboBox>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QMessageBox>
#include <QCloseEvent>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QStorageInfo>
#include <QThread>
#include <QTimer>
#include <QKeyEvent>
#include <QPainter>
#include <QDir>
#include <QElapsedTimer>
#include <QTime>
#include <QSet>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstring>

#ifdef Q_OS_WIN
#include <windows.h>
#include <setupapi.h>
#endif

// ── palette (matches SystemInfoWidget) ──
static const char *TB_PAGE_BG = "#eef2f5";
static const char *TB_CARD_BG = "#ffffff";
static const char *TB_BORDER  = "#c8d8e8";
static const char *TB_FG      = "#2a3a4a";
static const char *TB_FG_DIM  = "#6a7a8a";
static const char *TB_ACCENT  = "#5C7C98";

// Callback a test dialog uses to hand its result back to the tab (name, summary).
using ResultRecorder = std::function<void(const QString&, const QString&)>;

// Accent-styled action button; used for the "Start" buttons inside the test
// dialogs so the parent tab's flat QWidget background rule can't wash them out.
static void styleActionButton(QPushButton *btn)
{
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumHeight(36);
    btn->setStyleSheet(QString(
        "QPushButton{background:%1; color:white; border:none; border-radius:6px;"
        " font-size:13px; font-weight:bold; padding:6px 14px;}"
        "QPushButton:hover{background:#4a6880;}"
        "QPushButton:disabled{background:#B9C4CE; color:#EAEFF3;}").arg(TB_ACCENT));
}

// ════════════════════════════════════════════════════════════
//  Dead-pixel / backlight test — fullscreen solid-color cycle
// ════════════════════════════════════════════════════════════
namespace {
class DeadPixelDialog : public QDialog {
public:
    explicit DeadPixelDialog(const QString &hint, QWidget *parent = nullptr)
        : QDialog(parent), m_hint(hint)
    {
        setWindowTitle("Dead Pixel Test");
        setWindowState(windowState() | Qt::WindowFullScreen);
        setCursor(Qt::BlankCursor);
        m_colors = { Qt::red, Qt::green, Qt::blue, Qt::white,
                     QColor(0, 0, 0), QColor(128, 128, 128) };
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), m_colors[m_index]);

        // Hint text in a readable contrast color, only on the first color.
        if (m_showHint) {
            p.setPen(m_index <= 2 || m_index == 4 ? Qt::white : Qt::black);
            QFont f = font();
            f.setPointSize(16);
            p.setFont(f);
            p.drawText(rect().adjusted(0, 0, 0, -40),
                       Qt::AlignHCenter | Qt::AlignBottom, m_hint);
        }
    }

    void mousePressEvent(QMouseEvent *) override { advance(); }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) { accept(); return; }
        advance();
    }

private:
    void advance()
    {
        m_showHint = false;
        m_index = (m_index + 1) % m_colors.size();
        update();
    }

    QList<QColor> m_colors;
    int m_index = 0;
    bool m_showHint = true;
    QString m_hint;
};
} // namespace

// ════════════════════════════════════════════════════════════
//  Disk speed benchmark — sequential write + read, cache-bypassed
// ════════════════════════════════════════════════════════════
namespace {
struct BenchResult { bool ok; double writeMBps; double readMBps; QString error; };
} // namespace
Q_DECLARE_METATYPE(BenchResult)

namespace {
class DiskBenchWorker : public QObject {
    Q_OBJECT
public:
    DiskBenchWorker(const QString &volumePath, qint64 testBytes)
        : m_volume(volumePath), m_testBytes(testBytes) {}

public slots:
    void run()
    {
        BenchResult r{false, 0, 0, QString()};
#ifdef Q_OS_WIN
        const DWORD sector = 4096;                 // safe alignment for NO_BUFFERING
        const DWORD block  = 8 * 1024 * 1024;      // 8 MiB I/O blocks
        QString filePath = QDir(m_volume).filePath("uqr_disktest.tmp");
        std::wstring wpath = QDir::toNativeSeparators(filePath).toStdWString();

        // Aligned buffer required by FILE_FLAG_NO_BUFFERING.
        void *buf = _aligned_malloc(block, sector);
        if (!buf) { r.error = "Out of memory for benchmark buffer."; emit finished(r); return; }
        memset(buf, 0xA5, block);

        auto cleanup = [&]() {
            if (buf) { _aligned_free(buf); buf = nullptr; }
            DeleteFileW(wpath.c_str());
        };

        // ── sequential write ──
        HANDLE hw = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hw == INVALID_HANDLE_VALUE) {
            r.error = QString("Cannot create test file on %1 (error %2).").arg(m_volume).arg(GetLastError());
            cleanup(); emit finished(r); return;
        }
        QElapsedTimer t; t.start();
        qint64 written = 0;
        while (written < m_testBytes) {
            DWORD w = 0;
            if (!WriteFile(hw, buf, block, &w, NULL) || w == 0) {
                r.error = QString("Write failed at %1 MB (error %2).").arg(written / (1024*1024)).arg(GetLastError());
                CloseHandle(hw); cleanup(); emit finished(r); return;
            }
            written += w;
            emit progress(int((written * 50) / m_testBytes), tr("Writing..."));
        }
        double writeSec = t.nsecsElapsed() / 1e9;
        CloseHandle(hw);
        r.writeMBps = writeSec > 0 ? (written / (1024.0*1024.0)) / writeSec : 0;

        // ── sequential read (cache bypassed) ──
        HANDLE hr = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hr == INVALID_HANDLE_VALUE) {
            r.error = QString("Cannot reopen test file for reading (error %1).").arg(GetLastError());
            cleanup(); emit finished(r); return;
        }
        t.restart();
        qint64 read = 0;
        while (read < written) {
            DWORD got = 0;
            if (!ReadFile(hr, buf, block, &got, NULL)) {
                r.error = QString("Read failed at %1 MB (error %2).").arg(read / (1024*1024)).arg(GetLastError());
                CloseHandle(hr); cleanup(); emit finished(r); return;
            }
            if (got == 0) break;
            read += got;
            emit progress(50 + int((read * 50) / written), tr("Reading..."));
        }
        double readSec = t.nsecsElapsed() / 1e9;
        CloseHandle(hr);
        r.readMBps = readSec > 0 ? (read / (1024.0*1024.0)) / readSec : 0;

        cleanup();
        r.ok = true;
#else
        r.error = "Not supported on this OS.";
#endif
        emit finished(r);
    }

signals:
    void progress(int percent, const QString &phase);
    void finished(const BenchResult &result);

private:
    QString m_volume;
    qint64 m_testBytes;
};

class DiskBenchDialog : public QDialog {
public:
    DiskBenchDialog(std::function<QString(const QString&)> tr, ResultRecorder rec, QWidget *parent = nullptr)
        : QDialog(parent), tr_(tr), rec_(rec)
    {
        qRegisterMetaType<BenchResult>("BenchResult");
        setWindowTitle(tr_("test_disk_speed"));
        resize(460, 300);
        auto *vl = new QVBoxLayout(this);
        vl->setSpacing(10);

        vl->addWidget(new QLabel(tr_("disk_speed_pick")));
        m_combo = new QComboBox(this);
        for (const QStorageInfo &s : QStorageInfo::mountedVolumes()) {
            if (!s.isValid() || !s.isReady() || s.isReadOnly()) continue;
            if (s.bytesAvailable() < qint64(2) * 1024 * 1024 * 1024) continue; // need >2 GB free
            QString root = s.rootPath();
            QString label = s.displayName().isEmpty() ? root : QString("%1 (%2)").arg(root, s.displayName());
            m_combo->addItem(label, root);
        }
        vl->addWidget(m_combo);

        m_startBtn = new QPushButton(tr_("disk_speed_start"), this);
        styleActionButton(m_startBtn);
        vl->addWidget(m_startBtn);

        m_bar = new QProgressBar(this);
        m_bar->setRange(0, 100);
        m_bar->setValue(0);
        vl->addWidget(m_bar);

        m_phase = new QLabel(" ", this);
        vl->addWidget(m_phase);

        m_result = new QLabel(this);
        m_result->setStyleSheet(QString("font-size:15px; font-weight:bold; color:%1;").arg(TB_FG));
        m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        vl->addWidget(m_result);
        vl->addStretch();

        if (m_combo->count() == 0) {
            m_startBtn->setEnabled(false);
            m_phase->setText(tr_("disk_speed_no_volume"));
        }
        connect(m_startBtn, &QPushButton::clicked, this, &DiskBenchDialog::start);
    }

private:
    void start()
    {
        if (m_combo->count() == 0) return;
        m_startBtn->setEnabled(false);
        m_combo->setEnabled(false);
        m_result->clear();
        m_bar->setValue(0);

        // 1 GB test, but never more than a quarter of the free space.
        QString root = m_combo->currentData().toString();
        QStorageInfo info(root);
        qint64 testBytes = qMin<qint64>(qint64(1) * 1024 * 1024 * 1024, info.bytesAvailable() / 4);
        testBytes = (testBytes / (8 * 1024 * 1024)) * (8 * 1024 * 1024); // multiple of block

        auto *thread = new QThread(this);
        auto *worker = new DiskBenchWorker(root, testBytes);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &DiskBenchWorker::run);
        connect(worker, &DiskBenchWorker::progress, this, [this](int p, const QString &phase) {
            m_bar->setValue(p);
            m_phase->setText(phase);
        });
        connect(worker, &DiskBenchWorker::finished, this, [this, thread, worker](const BenchResult &r) {
            thread->quit();
            thread->wait();
            worker->deleteLater();
            thread->deleteLater();

            m_startBtn->setEnabled(true);
            m_combo->setEnabled(true);
            m_bar->setValue(100);
            if (r.ok) {
                m_phase->setText(tr_("disk_speed_done"));
                m_result->setText(QString("%1: %2 MB/s\n%3: %4 MB/s")
                                      .arg(tr_("disk_speed_write"))
                                      .arg(r.writeMBps, 0, 'f', 0)
                                      .arg(tr_("disk_speed_read"))
                                      .arg(r.readMBps, 0, 'f', 0));
                if (rec_)
                    rec_(tr_("test_disk_speed"), QString("%1 — %2: %3 MB/s, %4: %5 MB/s")
                             .arg(m_combo->currentData().toString())
                             .arg(tr_("disk_speed_write")).arg(r.writeMBps, 0, 'f', 0)
                             .arg(tr_("disk_speed_read")).arg(r.readMBps, 0, 'f', 0));
            } else {
                m_phase->setText(" ");
                m_result->setStyleSheet("font-size:13px; font-weight:bold; color:#cc2222;");
                m_result->setText(r.error);
            }
        });
        thread->start();
    }

    std::function<QString(const QString&)> tr_;
    ResultRecorder rec_;
    QComboBox *m_combo;
    QPushButton *m_startBtn;
    QProgressBar *m_bar;
    QLabel *m_phase;
    QLabel *m_result;
};
} // namespace

// ════════════════════════════════════════════════════════════
//  USB port test — poll for ANY USB device arrival/removal
//  (use a mouse or keyboard; no need to risk a storage drive)
// ════════════════════════════════════════════════════════════
namespace {
class UsbPortDialog : public QDialog {
public:
    UsbPortDialog(std::function<QString(const QString&)> tr, ResultRecorder rec, QWidget *parent = nullptr)
        : QDialog(parent), tr_(tr), rec_(rec)
    {
        setWindowTitle(tr_("test_usb_ports"));
        resize(500, 400);
        auto *vl = new QVBoxLayout(this);

        auto *info = new QLabel(tr_("usb_test_hint"), this);
        info->setWordWrap(true);
        info->setStyleSheet(QString("color:%1;").arg(TB_FG_DIM));
        vl->addWidget(info);

        m_count = new QLabel(this);
        m_count->setStyleSheet(QString("font-size:15px; font-weight:bold; color:%1;").arg(TB_ACCENT));
        vl->addWidget(m_count);

        m_log = new QPlainTextEdit(this);
        m_log->setReadOnly(true);
        vl->addWidget(m_log, 1);

        m_current = usbDevices();
        updateCount();

        m_timer = new QTimer(this);
        m_timer->setInterval(500);
        connect(m_timer, &QTimer::timeout, this, &UsbPortDialog::poll);
        m_timer->start();
    }

protected:
    void closeEvent(QCloseEvent *e) override
    {
        if (rec_ && !m_everSeen.isEmpty())
            rec_(tr_("test_usb_ports"),
                 tr_("usb_ports_seen").replace("{n}", QString::number(m_everSeen.size())));
        QDialog::closeEvent(e);
    }

private:
    // Enumerates every present USB device (mouse, keyboard, flash drive, hub, …)
    // keyed by device instance ID, valued by a human-readable name.
    QMap<QString, QString> usbDevices()
    {
        QMap<QString, QString> map;
#ifdef Q_OS_WIN
        // GUID_DEVINTERFACE_USB_DEVICE {A5DCBF10-6530-11D2-901F-00C04FB951ED}
        static const GUID kUsbDeviceGuid =
            { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

        HDEVINFO info = SetupDiGetClassDevsW(&kUsbDeviceGuid, nullptr, nullptr,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            return map;

        SP_DEVINFO_DATA dev;
        dev.cbSize = sizeof(dev);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(info, i, &dev); ++i) {
            wchar_t idBuf[512] = {};
            if (!SetupDiGetDeviceInstanceIdW(info, &dev, idBuf, 512, nullptr))
                continue;
            QString instanceId = QString::fromWCharArray(idBuf);

            wchar_t nameBuf[512] = {};
            DWORD type = 0;
            // Prefer friendly name, fall back to device description.
            if (!SetupDiGetDeviceRegistryPropertyW(info, &dev, SPDRP_FRIENDLYNAME, &type,
                                                   (PBYTE)nameBuf, sizeof(nameBuf), nullptr)) {
                SetupDiGetDeviceRegistryPropertyW(info, &dev, SPDRP_DEVICEDESC, &type,
                                                  (PBYTE)nameBuf, sizeof(nameBuf), nullptr);
            }
            QString name = QString::fromWCharArray(nameBuf).trimmed();
            if (name.isEmpty()) name = "USB Device";
            map.insert(instanceId, name);
        }
        SetupDiDestroyDeviceInfoList(info);
#endif
        return map;
    }

    void poll()
    {
        QMap<QString, QString> now = usbDevices();
        for (auto it = now.constBegin(); it != now.constEnd(); ++it) {
            if (!m_current.contains(it.key())) {
                m_everSeen.insert(it.key());
                logLine(QString("[+] %1  (%2)").arg(it.value(), tr_("usb_connected")));
            }
        }
        for (auto it = m_current.constBegin(); it != m_current.constEnd(); ++it) {
            if (!now.contains(it.key()))
                logLine(QString("[-] %1  (%2)").arg(it.value(), tr_("usb_removed")));
        }
        m_current = now;
        updateCount();
    }

    void logLine(const QString &s)
    {
        m_log->appendPlainText(QTime::currentTime().toString("HH:mm:ss") + "  " + s);
    }

    void updateCount()
    {
        m_count->setText(tr_("usb_ports_seen").replace("{n}", QString::number(m_everSeen.size()))
                         + "   |   " + tr_("usb_now_connected").replace("{n}", QString::number(m_current.size())));
    }

    std::function<QString(const QString&)> tr_;
    ResultRecorder rec_;
    QLabel *m_count;
    QPlainTextEdit *m_log;
    QTimer *m_timer;
    QMap<QString, QString> m_current;
    QSet<QString> m_everSeen;
};
} // namespace

// ════════════════════════════════════════════════════════════
//  CPU benchmark & stress test — all-core throughput
// ════════════════════════════════════════════════════════════
namespace {
struct CpuResult { bool ok; double singleScore; double multiScore; int threads; QString error; };
} // namespace
Q_DECLARE_METATYPE(CpuResult)

namespace {
class CpuBenchWorker : public QObject {
    Q_OBJECT
public:
    explicit CpuBenchWorker(int perPhaseSeconds) : m_seconds(perPhaseSeconds) {}

public slots:
    void run()
    {
        CpuResult r{true, 0, 0, 0, QString()};
        const int hw = int((std::max)(1u, std::thread::hardware_concurrency()));
        r.threads = hw;
        // Single-core score first, then all-core; each phase fills half the bar.
        r.singleScore = measure(1,  0,  50, "single");
        r.multiScore  = measure(hw, 50, 50, "multi");
        emit finished(r);
    }

signals:
    void progress(int percent, const QString &phase);
    void finished(const CpuResult &result);

private:
    // Runs an integer LCG workload on nThreads for m_seconds and returns the
    // aggregate rate in "million iterations per second" (a relative score).
    double measure(int nThreads, int baseProgress, int spanProgress, const QString &phase)
    {
        std::atomic<bool> stop{false};
        std::atomic<unsigned long long> totalIters{0};
        std::atomic<unsigned long long> sink{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < nThreads; ++t) {
            threads.emplace_back([&]() {
                unsigned long long iters = 0;
                unsigned long long acc = 0x9E3779B97F4A7C15ULL + t;
                while (!stop.load(std::memory_order_relaxed)) {
                    for (int k = 0; k < 200000; ++k) {
                        acc = acc * 6364136223846793005ULL + 1442695040888963407ULL;
                        ++iters;
                    }
                }
                totalIters.fetch_add(iters, std::memory_order_relaxed);
                sink.fetch_xor(acc, std::memory_order_relaxed); // keep acc observable
            });
        }

        QElapsedTimer et; et.start();
        const qint64 ms = qint64(m_seconds) * 1000;
        while (et.elapsed() < ms) {
            QThread::msleep(100);
            emit progress(baseProgress + int(spanProgress * et.elapsed() / ms), phase);
        }
        stop.store(true, std::memory_order_relaxed);
        for (auto &th : threads) th.join();

        const double sec = et.elapsed() / 1000.0;
        return sec > 0 ? (double(totalIters.load()) / sec / 1e6) : 0.0;
    }

    int m_seconds;
};

class CpuTestDialog : public QDialog {
public:
    CpuTestDialog(std::function<QString(const QString&)> tr, ResultRecorder rec, QWidget *parent = nullptr)
        : QDialog(parent), tr_(tr), rec_(rec)
    {
        qRegisterMetaType<CpuResult>("CpuResult");
        setWindowTitle(tr_("test_cpu"));
        resize(460, 300);
        auto *vl = new QVBoxLayout(this);
        vl->setSpacing(10);

        auto *info = new QLabel(tr_("cpu_test_hint"), this);
        info->setWordWrap(true);
        info->setStyleSheet(QString("color:%1;").arg(TB_FG_DIM));
        vl->addWidget(info);

        vl->addWidget(new QLabel(tr_("cpu_test_duration")));
        m_combo = new QComboBox(this);
        m_combo->addItem("10 s", 5);
        m_combo->addItem("30 s", 15);
        m_combo->addItem("60 s", 30);
        vl->addWidget(m_combo);

        m_startBtn = new QPushButton(tr_("cpu_test_start"), this);
        styleActionButton(m_startBtn);
        vl->addWidget(m_startBtn);

        m_bar = new QProgressBar(this);
        m_bar->setRange(0, 100);
        vl->addWidget(m_bar);

        m_phase = new QLabel(" ", this);
        vl->addWidget(m_phase);

        m_result = new QLabel(this);
        m_result->setStyleSheet(QString("font-size:14px; font-weight:bold; color:%1;").arg(TB_FG));
        m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_result->setWordWrap(true);
        vl->addWidget(m_result);
        vl->addStretch();

        connect(m_startBtn, &QPushButton::clicked, this, &CpuTestDialog::start);
    }

private:
    void start()
    {
        m_startBtn->setEnabled(false);
        m_combo->setEnabled(false);
        m_result->clear();
        m_result->setStyleSheet(QString("font-size:14px; font-weight:bold; color:%1;").arg(TB_FG));
        m_bar->setValue(0);

        const int perPhase = m_combo->currentData().toInt();
        auto *thread = new QThread(this);
        auto *worker = new CpuBenchWorker(perPhase);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &CpuBenchWorker::run);
        connect(worker, &CpuBenchWorker::progress, this, [this](int p, const QString &phase) {
            m_bar->setValue(p);
            m_phase->setText(phase == "single" ? tr_("cpu_phase_single") : tr_("cpu_phase_multi"));
        });
        connect(worker, &CpuBenchWorker::finished, this, [this, thread, worker](const CpuResult &r) {
            thread->quit(); thread->wait();
            worker->deleteLater(); thread->deleteLater();

            m_startBtn->setEnabled(true);
            m_combo->setEnabled(true);
            m_bar->setValue(100);
            m_phase->setText(tr_("cpu_test_done"));

            double scaling = r.singleScore > 0 ? r.multiScore / r.singleScore : 0.0;
            m_result->setText(QString("%1: %2\n%3: %4  (%5 %6, ×%7)")
                                  .arg(tr_("cpu_single_score")).arg(r.singleScore, 0, 'f', 0)
                                  .arg(tr_("cpu_multi_score")).arg(r.multiScore, 0, 'f', 0)
                                  .arg(r.threads).arg(tr_("cpu_threads_label"))
                                  .arg(scaling, 0, 'f', 1));
            if (rec_)
                rec_(tr_("test_cpu"), QString("%1 %2 / %3 %4 (%5 %6)")
                         .arg(tr_("cpu_single_score")).arg(r.singleScore, 0, 'f', 0)
                         .arg(tr_("cpu_multi_score")).arg(r.multiScore, 0, 'f', 0)
                         .arg(r.threads).arg(tr_("cpu_threads_label")));
        });
        thread->start();
    }

    std::function<QString(const QString&)> tr_;
    ResultRecorder rec_;
    QComboBox *m_combo;
    QPushButton *m_startBtn;
    QProgressBar *m_bar;
    QLabel *m_phase;
    QLabel *m_result;
};
} // namespace

// ════════════════════════════════════════════════════════════
//  RAM test — pattern write/verify to catch bad memory
// ════════════════════════════════════════════════════════════
namespace {
struct RamResult { bool ok; qint64 bytesTested; int passes; qint64 errors; QString firstError; QString error; };
} // namespace
Q_DECLARE_METATYPE(RamResult)

namespace {
class RamTestWorker : public QObject {
    Q_OBJECT
public:
    explicit RamTestWorker(qint64 bytes) : m_bytes(bytes) {}

public slots:
    void run()
    {
        RamResult r{false, 0, 0, 0, QString(), QString()};

        const qint64 count = m_bytes / qint64(sizeof(quint64));
        quint64 *buf = static_cast<quint64*>(malloc(size_t(count) * sizeof(quint64)));
        if (!buf) {
            r.error = "Could not allocate the requested memory. Try a smaller size.";
            emit finished(r);
            return;
        }

        // Fixed patterns plus an address-based pass (catches stuck address lines).
        const quint64 patterns[] = {
            0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
            0x5555555555555555ULL, 0xAAAAAAAAAAAAAAAAULL,
        };
        const int totalPasses = int(sizeof(patterns) / sizeof(patterns[0])) + 1; // +1 address pass
        int passIndex = 0;

        auto runPass = [&](auto valueFor) {
            // write
            for (qint64 i = 0; i < count; ++i) buf[i] = valueFor(i);
            // verify
            for (qint64 i = 0; i < count; ++i) {
                if (buf[i] != valueFor(i)) {
                    ++r.errors;
                    if (r.firstError.isEmpty())
                        r.firstError = QString("offset %1: wrote %2, read %3")
                                           .arg(i * sizeof(quint64))
                                           .arg(valueFor(i), 16, 16, QChar('0'))
                                           .arg(buf[i], 16, 16, QChar('0'));
                }
                if ((i & 0xFFFFF) == 0) {
                    int base = int((qint64(passIndex) * 100) / totalPasses);
                    int within = int((i * 100 / qMax<qint64>(count, 1)) / totalPasses);
                    emit progress(base + within);
                }
            }
            ++passIndex;
        };

        for (quint64 p : patterns) {
            runPass([p](qint64) { return p; });
            emit progress(int((qint64(passIndex) * 100) / totalPasses));
        }
        // address pass
        runPass([](qint64 i) { return quint64(i) * 0x9E3779B97F4A7C15ULL; });

        free(buf);
        r.bytesTested = count * qint64(sizeof(quint64));
        r.passes = totalPasses;
        r.ok = true;
        emit progress(100);
        emit finished(r);
    }

signals:
    void progress(int percent);
    void finished(const RamResult &result);

private:
    qint64 m_bytes;
};

class RamTestDialog : public QDialog {
public:
    RamTestDialog(std::function<QString(const QString&)> tr, ResultRecorder rec, QWidget *parent = nullptr)
        : QDialog(parent), tr_(tr), rec_(rec)
    {
        qRegisterMetaType<RamResult>("RamResult");
        setWindowTitle(tr_("test_ram"));
        resize(460, 300);
        auto *vl = new QVBoxLayout(this);
        vl->setSpacing(10);

        auto *info = new QLabel(tr_("ram_test_hint"), this);
        info->setWordWrap(true);
        info->setStyleSheet(QString("color:%1;").arg(TB_FG_DIM));
        vl->addWidget(info);

        // Offer sizes up to (free physical RAM - 1 GB) to avoid heavy swapping.
        qint64 freeBytes = availablePhysicalBytes();
        qint64 cap = freeBytes - qint64(1) * 1024 * 1024 * 1024;

        vl->addWidget(new QLabel(tr_("ram_test_size")));
        m_combo = new QComboBox(this);
        const qint64 GB = qint64(1) * 1024 * 1024 * 1024;
        for (qint64 gb : { qint64(1), qint64(2), qint64(4), qint64(8) }) {
            if (gb * GB <= cap) m_combo->addItem(QString("%1 GB").arg(gb), gb * GB);
        }
        if (m_combo->count() == 0 && cap >= 256LL * 1024 * 1024)
            m_combo->addItem("512 MB", 512LL * 1024 * 1024);
        vl->addWidget(m_combo);

        m_startBtn = new QPushButton(tr_("ram_test_start"), this);
        styleActionButton(m_startBtn);
        vl->addWidget(m_startBtn);

        m_bar = new QProgressBar(this);
        m_bar->setRange(0, 100);
        vl->addWidget(m_bar);

        m_result = new QLabel(" ", this);
        m_result->setStyleSheet(QString("font-size:14px; font-weight:bold; color:%1;").arg(TB_FG));
        m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_result->setWordWrap(true);
        vl->addWidget(m_result);
        vl->addStretch();

        if (m_combo->count() == 0) {
            m_startBtn->setEnabled(false);
            m_result->setText(tr_("ram_test_no_mem"));
        }
        connect(m_startBtn, &QPushButton::clicked, this, &RamTestDialog::start);
    }

private:
    static qint64 availablePhysicalBytes()
    {
#ifdef Q_OS_WIN
        MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) return qint64(ms.ullAvailPhys);
#endif
        return 0;
    }

    void start()
    {
        if (m_combo->count() == 0) return;
        m_startBtn->setEnabled(false);
        m_combo->setEnabled(false);
        m_result->setStyleSheet(QString("font-size:14px; font-weight:bold; color:%1;").arg(TB_FG));
        m_result->setText(tr_("ram_test_running"));
        m_bar->setValue(0);

        const qint64 bytes = m_combo->currentData().toLongLong();
        auto *thread = new QThread(this);
        auto *worker = new RamTestWorker(bytes);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &RamTestWorker::run);
        connect(worker, &RamTestWorker::progress, this, [this](int p) { m_bar->setValue(p); });
        connect(worker, &RamTestWorker::finished, this, [this, thread, worker](const RamResult &r) {
            thread->quit(); thread->wait();
            worker->deleteLater(); thread->deleteLater();

            m_startBtn->setEnabled(true);
            m_combo->setEnabled(true);
            m_bar->setValue(100);

            if (!r.ok) {
                m_result->setStyleSheet("font-size:13px; font-weight:bold; color:#cc2222;");
                m_result->setText(r.error);
                return;
            }
            const double gb = r.bytesTested / (1024.0 * 1024 * 1024);
            if (r.errors == 0) {
                m_result->setStyleSheet("font-size:14px; font-weight:bold; color:#1a7a1a;");
                m_result->setText(tr_("ram_test_pass")
                                      .replace("{gb}", QString::number(gb, 'f', 1))
                                      .replace("{passes}", QString::number(r.passes)));
                if (rec_)
                    rec_(tr_("test_ram"), tr_("test_pass") + QString(" — %1 GB, %2 %3")
                             .arg(gb, 0, 'f', 1).arg(r.passes).arg(tr_("ram_patterns_label")));
            } else {
                m_result->setStyleSheet("font-size:13px; font-weight:bold; color:#cc2222;");
                m_result->setText(tr_("ram_test_fail")
                                      .replace("{errors}", QString::number(r.errors))
                                      .replace("{detail}", r.firstError));
                if (rec_)
                    rec_(tr_("test_ram"), tr_("test_fail") + QString(" — %1 %2")
                             .arg(r.errors).arg(tr_("ram_errors_label")));
            }
        });
        thread->start();
    }

    std::function<QString(const QString&)> tr_;
    ResultRecorder rec_;
    QComboBox *m_combo;
    QPushButton *m_startBtn;
    QProgressBar *m_bar;
    QLabel *m_result;
};
} // namespace

// ════════════════════════════════════════════════════════════
//  HardwareTestWidget
// ════════════════════════════════════════════════════════════
HardwareTestWidget::HardwareTestWidget(bool hasTouch,
                                       std::function<QString(const QString&)> translator,
                                       QWidget *parent)
    : QWidget(parent), tr_(std::move(translator)), m_hasTouch(hasTouch)
{
    // Scope the background to this widget only (object-name selector) so the rule
    // does not cascade into child dialogs and flatten their buttons/inputs.
    setObjectName("HwTestRoot");
    setStyleSheet(QString("QWidget#HwTestRoot{background:%1;}").arg(TB_PAGE_BG));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 18);
    outer->setSpacing(14);

    auto *title = new QLabel(tr_("test_tab_title"), this);
    title->setStyleSheet(QString("font-size:16px; font-weight:bold; color:%1;").arg(TB_FG));
    outer->addWidget(title);

    m_grid = new QGridLayout();
    m_grid->setSpacing(14);
    outer->addLayout(m_grid);
    outer->addStretch();

    // Build tiles. Column count = 2.
    struct Def { QString key, desc; void (HardwareTestWidget::*fn)(); bool show; };
    QList<Def> defs = {
        { "test_input",      "test_input_desc",      &HardwareTestWidget::launchInputTest,     true },
        { "test_cpu",        "test_cpu_desc",        &HardwareTestWidget::launchCpuTest,       true },
        { "test_ram",        "test_ram_desc",        &HardwareTestWidget::launchRamTest,       true },
        { "test_dead_pixel", "test_dead_pixel_desc", &HardwareTestWidget::launchDeadPixelTest, true },
        { "test_disk_speed", "test_disk_speed_desc", &HardwareTestWidget::launchDiskSpeedTest, true },
        { "test_usb_ports",  "test_usb_ports_desc",  &HardwareTestWidget::launchUsbPortTest,  true },
        { "test_camera",     "test_camera_desc",     &HardwareTestWidget::launchCamera,       true },
        { "test_sound",      "test_sound_desc",      &HardwareTestWidget::launchSoundSettings, true },
        { "test_touch",      "test_touch_desc",      &HardwareTestWidget::launchTouchTest,     m_hasTouch },
    };

    int row = 0, col = 0;
    for (const Def &d : defs) {
        if (!d.show) continue;
        QPushButton *btn = addTile(d.key, d.desc);
        auto fn = d.fn;
        connect(btn, &QPushButton::clicked, this, [this, fn]() { (this->*fn)(); });
        m_grid->addWidget(m_tiles.last().btn->parentWidget(), row, col);
        if (++col >= 2) { col = 0; ++row; }
    }
    m_grid->setColumnStretch(0, 1);
    m_grid->setColumnStretch(1, 1);
}

QPushButton *HardwareTestWidget::addTile(const QString &textKey, const QString &descKey)
{
    auto *card = new QFrame(this);
    card->setStyleSheet(QString("QFrame{background:%1; border:1px solid %2; border-radius:8px;}")
                            .arg(TB_CARD_BG, TB_BORDER));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(16, 14, 16, 14);
    cl->setSpacing(8);

    auto *btn = new QPushButton(tr_(textKey), card);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumHeight(38);
    btn->setStyleSheet(QString(
        "QPushButton{background:%1; color:white; border:none; border-radius:6px;"
        " font-size:13px; font-weight:bold; padding:6px 10px;}"
        "QPushButton:hover{background:#4a6880;}").arg(TB_ACCENT));
    cl->addWidget(btn);

    auto *desc = new QLabel(tr_(descKey), card);
    desc->setWordWrap(true);
    desc->setStyleSheet(QString("color:%1; font-size:11px; border:none;").arg(TB_FG_DIM));
    cl->addWidget(desc);

    m_tiles.append({btn, textKey, descKey, desc});
    return btn;
}

void HardwareTestWidget::retranslate()
{
    for (const Tile &t : m_tiles) {
        t.btn->setText(tr_(t.textKey));
        t.desc->setText(tr_(t.descKey));
    }
}

// ── launchers ──
void HardwareTestWidget::launchDeadPixelTest()
{
    auto *dlg = new DeadPixelDialog(tr_("dead_pixel_hint"), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    connect(dlg, &QDialog::finished, this, [this](int) { askAndRecord("test_dead_pixel"); });
    dlg->showFullScreen();
}

void HardwareTestWidget::launchDiskSpeedTest()
{
    auto rec = [this](const QString &n, const QString &v) { recordResult(n, v); };
    auto *dlg = new DiskBenchDialog(tr_, rec, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
}

void HardwareTestWidget::launchUsbPortTest()
{
    auto rec = [this](const QString &n, const QString &v) { recordResult(n, v); };
    auto *dlg = new UsbPortDialog(tr_, rec, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void HardwareTestWidget::launchCpuTest()
{
    auto rec = [this](const QString &n, const QString &v) { recordResult(n, v); };
    auto *dlg = new CpuTestDialog(tr_, rec, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
}

void HardwareTestWidget::launchRamTest()
{
    auto rec = [this](const QString &n, const QString &v) { recordResult(n, v); };
    auto *dlg = new RamTestDialog(tr_, rec, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowModality(Qt::WindowModal);
    dlg->show();
}

void HardwareTestWidget::launchCamera()
{
    QProcess::startDetached("explorer.exe", QStringList() << "microsoft.windows.camera:");
}

void HardwareTestWidget::launchSoundSettings()
{
    QDesktopServices::openUrl(QUrl("ms-settings:sound"));
}

void HardwareTestWidget::launchInputTest()
{
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr_("test_input"));
    dlg->setWindowFlags(Qt::Window);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(1000, 560);

    auto *lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(dlg);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea{background:transparent;}");
    auto *input = new InputTestWidget(dlg);
    scroll->setWidget(input);
    lay->addWidget(scroll);

    dlg->show();
    // Route every keystroke to the tester while its window is open.
    input->setFocus();
    input->grabKeyboard();
    connect(dlg, &QDialog::finished, this, [this](int) { askAndRecord("test_input"); });
}

void HardwareTestWidget::launchTouchTest()
{
    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr_("test_touch"));
    dlg->setWindowFlags(Qt::Window);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->resize(960, 640);
    auto *lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(new TouchTestWidget(dlg));
    connect(dlg, &QDialog::finished, this, [this](int) { askAndRecord("test_touch"); });
    dlg->show();
}

void HardwareTestWidget::recordResult(const QString &name, const QString &value)
{
    for (auto &pr : m_results) {
        if (pr.first == name) { pr.second = value; return; }
    }
    m_results.append(qMakePair(name, value));
}

void HardwareTestWidget::askAndRecord(const QString &nameKey)
{
    QMessageBox box(this);
    box.setWindowTitle(tr_(nameKey));
    box.setText(tr_("test_passfail_prompt"));
    QPushButton *pass = box.addButton(tr_("test_pass"), QMessageBox::AcceptRole);
    box.addButton(tr_("test_fail"), QMessageBox::RejectRole);
    box.exec();
    recordResult(tr_(nameKey), box.clickedButton() == pass ? tr_("test_pass") : tr_("test_fail"));
}

#include "HardwareTestWidget.moc"
