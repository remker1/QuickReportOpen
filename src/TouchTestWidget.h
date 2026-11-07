// =============================================================================
// TouchTestWidget.h - multi-touch drawing canvas (TouchCanvas) and a stand-alone test widget.
// =============================================================================
#ifndef TOUCHTESTWIDGET_H
#define TOUCHTESTWIDGET_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QImage>
#include <QMap>
#include <QPointF>

// Canvas that records touch (and mouse) strokes and shows live touch points.
class TouchCanvas : public QWidget {
    Q_OBJECT
public:
    explicit TouchCanvas(QWidget *parent = nullptr);

public slots:
    void clearCanvas();

signals:
    // Emitted whenever the set of active touch points changes.
    void touchInfo(int activeCount, int maxSimultaneous);

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    // Distinct colour for each touch id.
    QColor colorForId(int id) const;
    // Draws the stroke piece for one contact onto the persistent image.
    void drawSegment(int id, const QPointF &from, const QPointF &to);

    QImage m_canvas;                 // persistent strokes
    QMap<int, QPointF> m_active;     // id -> current position
    QMap<int, QPointF> m_last;       // id -> previous position (for line drawing)
    int m_maxSimultaneous = 0;
};

// Container: a maximized canvas with a compact control strip (help "?", clear,
// and full-screen toggle). Instructions and the live touch counter are tucked
// behind the "?" and only appear on hover or click/touch.
class TouchTestWidget : public QWidget {
    Q_OBJECT
public:
    explicit TouchTestWidget(QWidget *parent = nullptr);

private slots:
    // Updates the hover help text with the live/max contact counts.
    void updateHelp(int active, int maxSimultaneous);

private:
    TouchCanvas *m_canvas;
    QPushButton *m_helpBtn;
    QPushButton *m_clearBtn;
    QPushButton *m_fullscreenBtn;
    QString      m_info;
};

#endif // TOUCHTESTWIDGET_H
