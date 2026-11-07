// =============================================================================
// HardwareTestWidget.h - LEGACY tile launcher for individual tests.
//
// Not part of the current CMake target (replaced by hardwaretester/HardwareTesterWidget);
// kept for reference.
// =============================================================================
#ifndef HARDWARETESTWIDGET_H
#define HARDWARETESTWIDGET_H

#include <QWidget>
#include <QMap>
#include <QString>
#include <QList>
#include <QPair>

class QGridLayout;
class QPushButton;

// A tab that gathers all the interactive hardware tests in one place: camera,
// sound, touch, plus the built-in dead-pixel, disk-speed and USB-port tests.
// Each launcher opens its own dialog; the widget owns no persistent test state.
class HardwareTestWidget : public QWidget {
    Q_OBJECT
public:
    // hasTouch controls whether the touchscreen test tile is shown.
    // translator maps a translation key to the current language's string, so this
    // widget shares MainWindow's i18n without duplicating the tables.
    explicit HardwareTestWidget(bool hasTouch,
                                std::function<QString(const QString&)> translator,
                                QWidget *parent = nullptr);

    // Re-labels every tile after a language switch.
    void retranslate();

    // Results of tests the user has run this session (name -> summary), in the
    // order they were first produced. Fed into the PDF inspection report.
    QList<QPair<QString, QString>> testResults() const { return m_results; }

private:
    QPushButton *addTile(const QString &textKey, const QString &descKey);
    void recordResult(const QString &name, const QString &value);
    void askAndRecord(const QString &nameKey);

    void launchDeadPixelTest();
    void launchDiskSpeedTest();
    void launchUsbPortTest();
    void launchCpuTest();
    void launchRamTest();
    void launchInputTest();
    void launchCamera();
    void launchSoundSettings();
    void launchTouchTest();

    std::function<QString(const QString&)> tr_;
    bool m_hasTouch;
    QGridLayout *m_grid = nullptr;

    // key -> (button, descriptionLabelKey) so retranslate() can refresh both.
    struct Tile { QPushButton *btn; QString textKey; QString descKey; class QLabel *desc; };
    QList<Tile> m_tiles;

    QList<QPair<QString, QString>> m_results;
};

#endif // HARDWARETESTWIDGET_H
