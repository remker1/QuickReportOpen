// =============================================================================
// CameraTestModule.h - camera preview, format selection, snapshot and fps test.
// =============================================================================
#ifndef CAMERATESTMODULE_H
#define CAMERATESTMODULE_H

#include "TesterModule.h"

#include <QCamera>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoSink>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class Card;

// Paints frames straight from a QVideoSink. One path gives preview, mirror,
// honest measured fps (source frames, not vsync) and the snapshot image.
class CameraView : public QWidget {
    Q_OBJECT
public:
    explicit CameraView(QWidget *parent = nullptr);
    QVideoSink *sink() { return &m_sink; }
    void setMirrored(bool m) { m_mirrored = m; update(); }
    QImage lastImage() const { return m_lastImage; }
    QSize frameSize() const { return m_lastImage.size(); }
    // Frames received since the last call (for measured-fps counting).
    int takeFrameCount() { const int n = m_frames; m_frames = 0; return n; }
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QVideoSink m_sink;
    QImage m_lastImage;
    bool m_mirrored = false;
    int m_frames = 0;
};

class CameraTestModule : public TesterModule {
    Q_OBJECT
public:
    explicit CameraTestModule(Translator t, QWidget *parent = nullptr);

    void retranslate() override;
    void moduleEntered() override;
    void moduleLeft() override;

private:
    // Reason shown in the banner when no preview is possible.
    enum class BannerState { None, NoDevice, PermissionDenied, DeviceBusy, OtherError };

    // Re-reads available cameras into the combo box.
    void refreshDeviceList();
    // Starts preview from the selected camera and applies the chosen format.
    void startSelectedCamera();
    // Stops the preview and releases the device.
    void stopCamera();
    // Applies the resolution/fps picked in the format list.
    void applySelectedFormat();
    // Grabs the last frame and shows it as a thumbnail.
    void takeSnapshot();
    // Timer slot: shows resolution and measured fps.
    void updateStats();
    // Shows or hides the problem banner.
    void showBanner(BannerState state, const QString &detail = QString());

    QComboBox *m_deviceCombo;
    QListWidget *m_formatList;
    CameraView *m_view;
    QLabel *m_banner;
    QLabel *m_statsLabel;
    QLabel *m_snapshotLabel;
    QCheckBox *m_mirrorCheck;
    QPushButton *m_snapshotBtn;
    Card *m_previewCard, *m_formatCard;
    QTimer *m_statsTimer;

    QMediaDevices m_mediaDevices;
    QMediaCaptureSession m_session;
    std::unique_ptr<QCamera> m_camera;
    BannerState m_bannerState = BannerState::None;
    QString m_bannerDetail;
    bool m_active = false; // module page currently shown
};

#endif // CAMERATESTMODULE_H
