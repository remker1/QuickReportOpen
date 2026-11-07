// =============================================================================
// TouchTestWidget.cpp - touch canvas implementation.
// =============================================================================
#include "TouchTestWidget.h"

#include <QPainter>
#include <QTouchEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolTip>

// -----------------------------------------------------------------------------
// TouchCanvas
// -----------------------------------------------------------------------------

// Accepts touch events; the strokes image is white.
TouchCanvas::TouchCanvas(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    setMouseTracking(false);
    setMinimumHeight(200);
    setStyleSheet("");
}

// Clears strokes and contact bookkeeping.
void TouchCanvas::clearCanvas() {
    if (!m_canvas.isNull()) m_canvas.fill(Qt::white);
    m_active.clear();
    m_last.clear();
    update();
    emit touchInfo(0, m_maxSimultaneous);
}

// (see header)
QColor TouchCanvas::colorForId(int id) const {
    static const QColor palette[] = {
        QColor("#e74c3c"), QColor("#27ae60"), QColor("#2980b9"),
        QColor("#f39c12"), QColor("#8e44ad"), QColor("#16a085"),
        QColor("#d35400"), QColor("#2c3e50"), QColor("#c0392b"), QColor("#1abc9c")
    };
    if (id < 0) return QColor("#34495e"); // mouse fallback
    return palette[id % 10];
}

// (see header)
void TouchCanvas::drawSegment(int id, const QPointF &from, const QPointF &to) {
    if (m_canvas.isNull()) return;
    QPainter painter(&m_canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(colorForId(id), 3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawLine(from, to);
}

// Handles TouchBegin/Update/End: draws a trail per contact, tracks the number of
// simultaneous contacts and emits touchInfo().
bool TouchCanvas::event(QEvent *e) {
    switch (e->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd: {
        auto *te = static_cast<QTouchEvent *>(e);
        for (const QEventPoint &p : te->points()) {
            const int id = p.id();
            const QPointF pos = p.position();

            switch (p.state()) {
            case QEventPoint::Pressed:
                m_last[id] = pos;
                m_active[id] = pos;
                break;
            case QEventPoint::Updated:
            case QEventPoint::Stationary:
                if (m_last.contains(id))
                    drawSegment(id, m_last[id], pos);
                m_last[id] = pos;
                m_active[id] = pos;
                break;
            case QEventPoint::Released:
                m_active.remove(id);
                m_last.remove(id);
                break;
            default:
                break;
            }
        }
        if (m_active.size() > m_maxSimultaneous)
            m_maxSimultaneous = m_active.size();
        emit touchInfo(m_active.size(), m_maxSimultaneous);
        update();
        e->accept();
        return true;
    }
    default:
        break;
    }
    return QWidget::event(e);
}

// Re-creates the persistent white image at the new size and copies old strokes in.
void TouchCanvas::resizeEvent(QResizeEvent *e) {
    QImage fresh(size(), QImage::Format_ARGB32_Premultiplied);
    fresh.fill(Qt::white);
    if (!m_canvas.isNull()) {
        QPainter p(&fresh);
        p.drawImage(0, 0, m_canvas);
    }
    m_canvas = fresh;
    QWidget::resizeEvent(e);
}

void TouchCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this);
    if (!m_canvas.isNull())
        p.drawImage(0, 0, m_canvas);

    // Border.
    p.setPen(QPen(QColor("#bbbbbb"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rect().adjusted(0, 0, -1, -1));

    // Live touch markers.
    p.setRenderHint(QPainter::Antialiasing, true);
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it) {
        const QColor c = colorForId(it.key());
        const QPointF pos = it.value();

        p.setOpacity(0.30);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(pos, 42, 42);

        p.setOpacity(1.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 2));
        p.drawEllipse(pos, 42, 42);

        p.setPen(QPen(c, 1));
        QFont f = p.font();
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(pos.x() - 42, pos.y() - 42, 84, 84),
                   Qt::AlignCenter, QString::number(it.key()));
    }
    p.setOpacity(1.0);
}

// Mouse fallback (id = -1) so digitizers reporting as a mouse still draw.
void TouchCanvas::mousePressEvent(QMouseEvent *e) {
    m_last[-1] = e->position();
    m_active[-1] = e->position();
    emit touchInfo(m_active.size(), m_maxSimultaneous);
    update();
}

void TouchCanvas::mouseMoveEvent(QMouseEvent *e) {
    if (m_last.contains(-1)) {
        drawSegment(-1, m_last[-1], e->position());
        m_last[-1] = e->position();
        m_active[-1] = e->position();
        update();
    }
}

void TouchCanvas::mouseReleaseEvent(QMouseEvent *) {
    m_active.remove(-1);
    m_last.remove(-1);
    emit touchInfo(m_active.size(), m_maxSimultaneous);
    update();
}

// -----------------------------------------------------------------------------
// TouchTestWidget
// -----------------------------------------------------------------------------

// Builds the canvas with the compact control strip (help, clear, full screen).
TouchTestWidget::TouchTestWidget(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Compact control strip pinned to the top-right; the canvas takes all the rest.
    auto *strip = new QHBoxLayout();
    strip->setContentsMargins(4, 4, 4, 4);
    strip->setSpacing(4);
    strip->addStretch();

    const QString smallBtn =
        "QPushButton{background:#ecf0f1; border:1px solid #bdc3c7; border-radius:4px;"
        " padding:1px 6px; font-size:12px; color:#000000;}"
        "QPushButton:hover{background:#dfe6e9;}";

    m_helpBtn = new QPushButton("?", this);
    m_helpBtn->setFixedSize(24, 24);
    m_helpBtn->setStyleSheet(smallBtn);
    m_helpBtn->setCursor(Qt::WhatsThisCursor);
    strip->addWidget(m_helpBtn);

    m_clearBtn = new QPushButton("Clear", this);
    m_clearBtn->setFixedHeight(24);
    m_clearBtn->setStyleSheet(smallBtn);
    strip->addWidget(m_clearBtn);

    m_fullscreenBtn = new QPushButton(QString::fromUtf8("\xE2\x9B\xB6"), this); // ⛶
    m_fullscreenBtn->setFixedSize(24, 24);
    m_fullscreenBtn->setStyleSheet(smallBtn);
    m_fullscreenBtn->setToolTip("Full Screen");
    strip->addWidget(m_fullscreenBtn);

    layout->addLayout(strip);

    m_canvas = new TouchCanvas(this);
    layout->addWidget(m_canvas, 1);

    updateHelp(0, 0);

    connect(m_clearBtn, &QPushButton::clicked, m_canvas, &TouchCanvas::clearCanvas);
    connect(m_canvas, &TouchCanvas::touchInfo, this, &TouchTestWidget::updateHelp);

    // Show the help text on click/touch (hover is handled by the tooltip).
    connect(m_helpBtn, &QPushButton::clicked, this, [this]() {
        QToolTip::showText(m_helpBtn->mapToGlobal(QPoint(0, m_helpBtn->height())),
                           m_info, m_helpBtn);
    });

    // Toggle full screen on the hosting window.
    connect(m_fullscreenBtn, &QPushButton::clicked, this, [this]() {
        if (QWidget *w = window()) {
            if (w->isFullScreen()) w->showNormal();
            else                   w->showFullScreen();
        }
    });
}

// (see header)
void TouchTestWidget::updateHelp(int active, int maxSimultaneous) {
    m_info = QString(
        "Touch and drag anywhere to test the digitizer.\n"
        "Use several fingers at once to test multi-touch.\n\n"
        "Active touches: %1 (max %2)").arg(active).arg(maxSimultaneous);
    m_helpBtn->setToolTip(m_info);
}
