// =============================================================================
// CloneProgressDialog.h - progress UI for the disk clone (see class comment below).
// =============================================================================
#ifndef CLONEPROGRESSDIALOG_H
#define CLONEPROGRESSDIALOG_H

#include <QDialog>
#include <QElapsedTimer>
#include <QString>
#include <functional>

class QLabel;
class QProgressBar;
class QCheckBox;
class QPushButton;

// Styled progress dialog for the system clone: source/target disk cards, a
// large percentage readout, a stage line, stat tiles (copied / speed / ETA /
// elapsed) and the shutdown-after-success option. Cancel semantics match the
// QProgressDialog it replaces: canceled() fires once and the dialog stays
// open until the worker confirms through cloneFinished.
// (Details of each piece are documented on the members below.)
class CloneProgressDialog : public QDialog {
    Q_OBJECT

public:
    // Translator: maps a translation key to display text (MainWindow::getTranslation).
    using Translator = std::function<QString(const QString &)>;
    explicit CloneProgressDialog(const Translator &tr, QWidget *parent = nullptr);

    // Fills the two disk cards: bold title ("Source: Disk 0") + detail (model, size).
    void setDisks(const QString &sourceTitle, const QString &sourceDetail,
                  const QString &targetTitle, const QString &targetDetail);
    // Sets the one-line phase text under the percentage (ignored while cancelling).
    void setStage(const QString &text);
    // True if the user ticked "shut down after success" (read when the clone ends).
    bool shutdownRequested() const;

public slots:
    // Updates the bar and large percent label (0-100).
    void setValue(int percent);
    // Updates percent + copied/total bytes, speed (MiB/s), ETA and elapsed time.
    void setDetail(int percent, qint64 copiedBytes, qint64 totalBytes,
                   double mibPerSec, qint64 etaSecs);

signals:
    // Emitted once when the user requests cancel (button, [X] or Esc).
    void canceled();

protected:
    // [X] and Esc both route through the cancel flow instead of closing; only
    // the programmatic close from the finish handler (signals blocked) passes.
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    // Builds one rounded card with a title and detail label.
    QWidget *makeDiskCard(QLabel **title, QLabel **detail);
    // Marks cancelling, disables the button, shows the notice and emits canceled().
    void requestCancel();

    Translator tr_;
    QLabel *m_srcTitle = nullptr;
    QLabel *m_srcDetail = nullptr;
    QLabel *m_tgtTitle = nullptr;
    QLabel *m_tgtDetail = nullptr;
    QLabel *m_percentLabel = nullptr;
    QLabel *m_stageLabel = nullptr;
    QProgressBar *m_bar = nullptr;
    QLabel *m_copiedValue = nullptr;
    QLabel *m_speedValue = nullptr;
    QLabel *m_etaValue = nullptr;
    QLabel *m_elapsedValue = nullptr;
    QCheckBox *m_shutdownCheck = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    // started in the constructor; drives the "Elapsed" tile
    QElapsedTimer m_elapsed;
    bool m_cancelRequested = false;
};

#endif // CLONEPROGRESSDIALOG_H
