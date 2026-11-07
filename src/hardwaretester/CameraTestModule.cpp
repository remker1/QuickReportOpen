// =============================================================================
// CameraTestModule.cpp - camera test page.
// =============================================================================
#include "CameraTestModule.h"
#include "common/Card.h"
#include "common/TesterTheme.h"

#include <QCameraDevice>
#include <QCameraFormat>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPermissions>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QVideoFrame>

// ── CameraView ─────────────────────────────────────────────────

// Counts every valid frame from the video sink, keeps it as an image and repaints.
CameraView::CameraView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    connect(&m_sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &f) {
        if (!f.isValid())
            return;
        ++m_frames;
        m_lastImage = f.toImage();
        update();
    });
}

// Drops the last frame (black view).
void CameraView::clear() {
    m_lastImage = QImage();
    m_frames = 0;
    update();
}

// Paints the last frame letter-boxed, mirrored if requested.
void CameraView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(24, 28, 33));
    if (m_lastImage.isNull())
        return;
    QImage img = m_mirrored ? m_lastImage.flipped(Qt::Horizontal) : m_lastImage;
    const QSize scaled = img.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2), scaled);
    p.drawImage(target, img);
}

// ── CameraTestModule ───────────────────────────────────────────

CameraTestModule::CameraTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(10);

    // Left: device + format list.
    auto *left = new QVBoxLayout();
    left->setSpacing(10);
    m_deviceCombo = new QComboBox(this);
    left->addWidget(m_deviceCombo);

    m_formatCard = new Card(QString(), this);
    m_formatList = new QListWidget(this);
    m_formatList->setFrameShape(QFrame::NoFrame);
    m_formatCard->body()->addWidget(m_formatList, 1);
    left->addWidget(m_formatCard, 1);
    outer->addLayout(left, 1);

    // Right: preview + stats + controls.
    auto *right = new QVBoxLayout();
    right->setSpacing(10);

    m_banner = new QLabel(this);
    m_banner->setWordWrap(true);
    m_banner->setVisible(false);
    m_banner->setStyleSheet(QString(
        "QLabel{background:%1;color:%2;border:1px solid %3;border-radius:8px;"
        "padding:10px;font-weight:600;}")
        .arg(TesterTheme::current().error.lighter(185).name(),
             TesterTheme::current().error.darker(120).name(),
             TesterTheme::current().error.name()));
    right->addWidget(m_banner);

    m_previewCard = new Card(QString(), this);
    m_view = new CameraView(this);
    m_previewCard->body()->addWidget(m_view, 1);

    m_statsLabel = new QLabel(this);
    m_previewCard->body()->addWidget(m_statsLabel);

    auto *controls = new QHBoxLayout();
    m_mirrorCheck = new QCheckBox(this);
    connect(m_mirrorCheck, &QCheckBox::toggled, this,
            [this](bool on) { m_view->setMirrored(on); });
    m_snapshotBtn = new QPushButton(this);
    connect(m_snapshotBtn, &QPushButton::clicked, this, &CameraTestModule::takeSnapshot);
    controls->addWidget(m_mirrorCheck);
    controls->addStretch();
    controls->addWidget(m_snapshotBtn);
    m_previewCard->body()->addLayout(controls);

    m_snapshotLabel = new QLabel(this);
    m_snapshotLabel->setWordWrap(true);
    m_snapshotLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_previewCard->body()->addWidget(m_snapshotLabel);

    right->addWidget(m_previewCard, 1);
    outer->addLayout(right, 2);

    m_session.setVideoSink(m_view->sink());

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &CameraTestModule::updateStats);

    connect(m_deviceCombo, &QComboBox::activated, this,
            [this](int) { startSelectedCamera(); });
    connect(m_formatList, &QListWidget::currentRowChanged, this,
            [this](int) { applySelectedFormat(); });
    connect(&m_mediaDevices, &QMediaDevices::videoInputsChanged, this, [this] {
        if (m_active)
            refreshDeviceList();
    });

    retranslate();
}

// Refreshes the device list and starts the preview.
void CameraTestModule::moduleEntered() {
    m_active = true;

    // Distinct, honest error states start with the permission check.
    QCameraPermission perm;
    switch (qApp->checkPermission(perm)) {
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(perm, this, [this](const QPermission &p) {
                if (!m_active)
                    return;
                if (p.status() == Qt::PermissionStatus::Granted)
                    refreshDeviceList();
                else
                    showBanner(BannerState::PermissionDenied);
            });
            return;
        case Qt::PermissionStatus::Denied:
            showBanner(BannerState::PermissionDenied);
            return;
        case Qt::PermissionStatus::Granted:
            break;
    }
    refreshDeviceList();
}

// Stops the camera so other apps can use it.
void CameraTestModule::moduleLeft() {
    m_active = false;
    stopCamera();
}

// (see header)
void CameraTestModule::refreshDeviceList() {
    const auto devices = QMediaDevices::videoInputs();
    m_deviceCombo->clear();
    for (const QCameraDevice &d : devices)
        m_deviceCombo->addItem(d.description(), QVariant::fromValue(d));

    if (devices.isEmpty()) {
        stopCamera();
        m_view->clear();
        m_formatList->clear();
        showBanner(BannerState::NoDevice);
        return;
    }
    showBanner(BannerState::None);
    startSelectedCamera();
}

// (see header) Checks camera permission/busy state and reports errors via the banner.
void CameraTestModule::startSelectedCamera() {
    stopCamera();
    const auto device = m_deviceCombo->currentData().value<QCameraDevice>();
    if (device.isNull())
        return;

    // Supported modes list.
    m_formatList->blockSignals(true);
    m_formatList->clear();
    for (const QCameraFormat &f : device.videoFormats()) {
        m_formatList->addItem(QString("%1 × %2   %3–%4 fps")
                                  .arg(f.resolution().width())
                                  .arg(f.resolution().height())
                                  .arg(qRound(f.minFrameRate()))
                                  .arg(qRound(f.maxFrameRate())));
        m_formatList->item(m_formatList->count() - 1)
            ->setData(Qt::UserRole, QVariant::fromValue(f));
    }
    m_formatList->blockSignals(false);

    m_camera = std::make_unique<QCamera>(device);
    connect(m_camera.get(), &QCamera::errorOccurred, this,
            [this](QCamera::Error error, const QString &text) {
                if (error == QCamera::NoError)
                    return;
                // Windows reports an occupied camera as CameraError with an
                // access/busy message; permission problems never get here
                // because we gate on checkPermission first.
                showBanner(BannerState::DeviceBusy, text);
            });
    m_session.setCamera(m_camera.get());
    m_camera->start();
    showBanner(BannerState::None);
    m_view->clear();
    m_statsTimer->start();
    updateStats();
}

// (see header)
void CameraTestModule::stopCamera() {
    m_statsTimer->stop();
    if (m_camera) {
        m_camera->stop();
        m_session.setCamera(nullptr);
        m_camera.reset();
    }
}

// (see header)
void CameraTestModule::applySelectedFormat() {
    if (!m_camera)
        return;
    auto *item = m_formatList->currentItem();
    if (!item)
        return;
    const auto fmt = item->data(Qt::UserRole).value<QCameraFormat>();
    if (fmt.isNull())
        return;
    m_camera->stop();
    m_camera->setCameraFormat(fmt);
    m_camera->start();
    m_view->clear();
}

// (see header) FPS = frames counted by CameraView per elapsed time.
void CameraTestModule::updateStats() {
    const int fps = m_view->takeFrameCount();
    QString nominal = "—";
    if (m_camera && !m_camera->cameraFormat().isNull())
        nominal = QString::number(qRound(m_camera->cameraFormat().maxFrameRate()));
    const QSize sz = m_view->frameSize();
    m_statsLabel->setText(tr_("ht_cam_stats")
                              .replace("{res}", sz.isValid() && !sz.isEmpty()
                                                    ? QString("%1×%2").arg(sz.width()).arg(sz.height())
                                                    : "—")
                              .replace("{nominal}", nominal)
                              .replace("{measured}", QString::number(fps)));
    if (m_camera && sz.isValid() && !sz.isEmpty())
        recordResult("ht_cam_result_name",
                     QString("%1 (%2×%3, %4 fps)")
                         .arg(m_deviceCombo->currentText())
                         .arg(sz.width()).arg(sz.height()).arg(fps));
}

// (see header)
void CameraTestModule::takeSnapshot() {
    const QImage img = m_view->lastImage();
    if (img.isNull())
        return;
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString path = QDir(dir).filePath(
        "QuickReports_camera_" +
        QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ".png");
    const QImage out = m_mirrorCheck->isChecked() ? img.flipped(Qt::Horizontal) : img;
    if (out.save(path, "PNG"))
        m_snapshotLabel->setText(tr_("ht_cam_snapshot_saved").replace("{path}", path));
    else
        m_snapshotLabel->setText(tr_("ht_cam_snapshot_failed"));
}

// (see header)
void CameraTestModule::showBanner(BannerState state, const QString &detail) {
    m_bannerState = state;
    m_bannerDetail = detail;
    switch (state) {
        case BannerState::None:
            m_banner->setVisible(false);
            return;
        case BannerState::NoDevice:
            m_banner->setText(tr_("ht_cam_no_device"));
            break;
        case BannerState::PermissionDenied:
            m_banner->setText(tr_("ht_cam_permission"));
            break;
        case BannerState::DeviceBusy:
        case BannerState::OtherError: {
            QString text = tr_("ht_cam_busy");
            if (!detail.isEmpty())
                text += "\n(" + detail + ")";
            m_banner->setText(text);
            break;
        }
    }
    m_banner->setVisible(true);
}

// Re-applies translated texts.
void CameraTestModule::retranslate() {
    m_formatCard->setTitle(tr_("ht_cam_formats"));
    m_previewCard->setTitle(tr_("ht_cam_preview"));
    m_mirrorCheck->setText(tr_("ht_cam_mirror"));
    m_snapshotBtn->setText(tr_("ht_cam_snapshot"));
    if (m_bannerState != BannerState::None)
        showBanner(m_bannerState, m_bannerDetail); // re-render banner text
    updateStats();
}
