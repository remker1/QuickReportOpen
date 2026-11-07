// =============================================================================
// InputTestWidget.cpp - LEGACY keyboard/trackpad test (not built; see header).
// =============================================================================
#include "InputTestWidget.h"
#include <QGridLayout>
#include <QFrame>
#include <QResizeEvent>
#include <QDebug>

// -----------------------------------------------------------------------------
// KeyLabel
// -----------------------------------------------------------------------------

// Creates a key cap with the three styles.
KeyLabel::KeyLabel(const QString &text, QWidget *parent) : QLabel(text, parent), isVisited(false) {
    setAlignment(Qt::AlignCenter);
    setFixedSize(35, 35); // compact so the whole keyboard fits without scrolling

    defaultStyle =
        "QLabel{background:#ffffff; border:1px solid #8b9bab; border-bottom:3px solid #6f8090;"
        " border-radius:6px; color:#2b3a48; font-size:11px; font-weight:600;}";
    pressedStyle =
        "QLabel{background:#5C7C98; border:1px solid #3d566b; border-bottom:3px solid #33495b;"
        " border-radius:6px; color:white; font-weight:bold; font-size:11px;}";
    visitedStyle =
        "QLabel{background:#c9ead3; border:1px solid #6bbf85; border-bottom:3px solid #55ac70;"
        " border-radius:6px; color:#22623a; font-weight:700; font-size:11px;}";

    setStyleSheet(defaultStyle);
}

// Pressed / released appearance; released keys stay marked visited.
void KeyLabel::setActive(bool active) {
    if (active) {
        setStyleSheet(pressedStyle);
        isVisited = true;
    } else {
        setStyleSheet(isVisited ? visitedStyle : defaultStyle);
    }
}

// -----------------------------------------------------------------------------
// TrackpadLabel
// -----------------------------------------------------------------------------

// Creates the trackpad area label.
TrackpadLabel::TrackpadLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    setAlignment(Qt::AlignCenter);
    setMouseTracking(true);

    defaultStyle = "QLabel{background:#ffffff; border:2px dashed #c3ccd6; border-radius:10px;"
                   " font-size:14px; color:#8a97a3;}";
    activeStyle  = "QLabel{background:#e8f6ee; border:2px solid #4caf7d; border-radius:10px;"
                   " font-size:14px; color:#2e8f5f; font-weight:bold;}";

    setStyleSheet(defaultStyle);
}

// Highlights while the pointer is moving over the area.
void TrackpadLabel::mouseMoveEvent(QMouseEvent *event) {
    setText(QString("Mouse: %1, %2").arg(event->pos().x()).arg(event->pos().y()));
}

// Highlights the clicked button.
void TrackpadLabel::mousePressEvent(QMouseEvent *event) {
    QString btn = "Unknown";
    if (event->button() == Qt::LeftButton) btn = "Left";
    else if (event->button() == Qt::RightButton) btn = "Right";
    else if (event->button() == Qt::MiddleButton) btn = "Middle";

    setText(btn + " Click");
    setStyleSheet(activeStyle);
}

// Restores the style.
void TrackpadLabel::mouseReleaseEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    setStyleSheet(defaultStyle);
    setText("Trackpad / Mouse Test Area");
}

// Shows the scroll direction.
void TrackpadLabel::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    setText(QString("Scroll ") + (delta > 0 ? "Up" : "Down"));
}

// -----------------------------------------------------------------------------
// InputTestWidget
// -----------------------------------------------------------------------------

namespace {
struct KeyDef {
    Qt::Key code;
    QString text;
    int width;         // pixel width (height is fixed)
    bool keypad = false;
};
}

// Builds the UI.
InputTestWidget::InputTestWidget(QWidget *parent) : QWidget(parent) {
    setupUi();
    setFocusPolicy(Qt::StrongFocus);
}

// Creates the keyboard rows, numpad and trackpad area.
void InputTestWidget::setupUi() {
    // Give the widget a light background so it never shows through as black,
    // and enable styled background so the rule applies to the widget itself.
    setObjectName("InputTestRoot");
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet("QWidget#InputTestRoot{background:#eef2f5;}");

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 12);
    outer->setSpacing(10);

    // ── Legend (centered) ─────────────────────────────────
    auto *legend = new QHBoxLayout();
    legend->setSpacing(16);
    legend->addStretch();
    auto addLegend = [&](const QString &color, const QString &border, const QString &text) {
        auto *box = new QLabel();
        box->setFixedSize(16, 16);
        box->setStyleSheet(QString("background:%1; border:1px solid %2; border-radius:4px;").arg(color, border));
        auto *lb = new QLabel(text);
        lb->setStyleSheet("color:#556; font-size:12px;");
        legend->addWidget(box);
        legend->addWidget(lb);
    };
    addLegend("#fbfcfd", "#c8d2dc", "Not pressed");
    addLegend("#5C7C98", "#47637c", "Pressed");
    addLegend("#e0f2e6", "#9fd3ac", "Tested");
    legend->addStretch();
    outer->addLayout(legend);

    // Helper to build a row of keys into a vertical layout. Actual sizes are set
    // dynamically in relayoutKeys() based on the available width.
    auto buildRows = [&](QVBoxLayout *dst, const QList<QList<KeyDef>> &rows, int spacing = 2) {
        for (const auto &row : rows) {
            auto *rl = new QHBoxLayout();
            rl->setSpacing(spacing);
            for (const auto &k : row) {
                auto *lbl = new KeyLabel(k.text, this);
                m_keys.append({lbl, k.width});
                rl->addWidget(lbl);
                int mapKey = k.keypad ? (static_cast<int>(k.code) | KEYPAD_FLAG)
                                      : static_cast<int>(k.code);
                keyMap[mapKey].append(lbl);
            }
            rl->addStretch();
            dst->addLayout(rl);
        }
    };

    // ── Main keyboard block ───────────────────────────────
    QList<QList<KeyDef>> mainRows = {
        { {Qt::Key_Escape,"Esc",46}, {Qt::Key_F1,"F1",46},{Qt::Key_F2,"F2",46},{Qt::Key_F3,"F3",46},{Qt::Key_F4,"F4",46},
          {Qt::Key_F5,"F5",46},{Qt::Key_F6,"F6",46},{Qt::Key_F7,"F7",46},{Qt::Key_F8,"F8",46},
          {Qt::Key_F9,"F9",46},{Qt::Key_F10,"F10",46},{Qt::Key_F11,"F11",46},{Qt::Key_F12,"F12",46} },
        { {Qt::Key_QuoteLeft,"`",46},{Qt::Key_1,"1",46},{Qt::Key_2,"2",46},{Qt::Key_3,"3",46},{Qt::Key_4,"4",46},
          {Qt::Key_5,"5",46},{Qt::Key_6,"6",46},{Qt::Key_7,"7",46},{Qt::Key_8,"8",46},{Qt::Key_9,"9",46},
          {Qt::Key_0,"0",46},{Qt::Key_Minus,"-",46},{Qt::Key_Equal,"=",46},{Qt::Key_Backspace,"Bksp",76} },
        { {Qt::Key_Tab,"Tab",70},{Qt::Key_Q,"Q",46},{Qt::Key_W,"W",46},{Qt::Key_E,"E",46},{Qt::Key_R,"R",46},
          {Qt::Key_T,"T",46},{Qt::Key_Y,"Y",46},{Qt::Key_U,"U",46},{Qt::Key_I,"I",46},{Qt::Key_O,"O",46},
          {Qt::Key_P,"P",46},{Qt::Key_BracketLeft,"[",46},{Qt::Key_BracketRight,"]",46},{Qt::Key_Backslash,"\\",52} },
        { {Qt::Key_CapsLock,"Caps",84},{Qt::Key_A,"A",46},{Qt::Key_S,"S",46},{Qt::Key_D,"D",46},{Qt::Key_F,"F",46},
          {Qt::Key_G,"G",46},{Qt::Key_H,"H",46},{Qt::Key_J,"J",46},{Qt::Key_K,"K",46},{Qt::Key_L,"L",46},
          {Qt::Key_Semicolon,";",46},{Qt::Key_Apostrophe,"'",46},{Qt::Key_Return,"Enter",88} },
        { {Qt::Key_Shift,"Shift",108},{Qt::Key_Z,"Z",46},{Qt::Key_X,"X",46},{Qt::Key_C,"C",46},{Qt::Key_V,"V",46},
          {Qt::Key_B,"B",46},{Qt::Key_N,"N",46},{Qt::Key_M,"M",46},{Qt::Key_Comma,",",46},{Qt::Key_Period,".",46},
          {Qt::Key_Slash,"/",46},{Qt::Key_Shift,"Shift",112} },
        { {Qt::Key_Control,"Ctrl",64},{Qt::Key_Meta,"Win",56},{Qt::Key_Alt,"Alt",56},{Qt::Key_Space,"Space",300},
          {Qt::Key_Alt,"Alt",56},{Qt::Key_Meta,"Win",56},{Qt::Key_Menu,"Menu",56},{Qt::Key_Control,"Ctrl",64} }
    };
    auto *mainCol = new QVBoxLayout();
    mainCol->setSpacing(3);
    buildRows(mainCol, mainRows);

    // ── Right cluster: nav keys, arrows, numpad ───────────
    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(3);

    // Nav 6-key block (Ins/Home/PgUp, Del/End/PgDn)
    QList<QList<KeyDef>> navRows = {
        { {Qt::Key_Print,"PrtSc",56},{Qt::Key_ScrollLock,"ScrLk",56},{Qt::Key_Pause,"Pause",56} },
        { {Qt::Key_Insert,"Ins",56},{Qt::Key_Home,"Home",56},{Qt::Key_PageUp,"PgUp",56} },
        { {Qt::Key_Delete,"Del",56},{Qt::Key_End,"End",56},{Qt::Key_PageDown,"PgDn",56} },
    };
    buildRows(rightCol, navRows);

    // Arrow cluster (inverted-T), centered so the up key sits above the down key.
    {
        auto *upRow = new QHBoxLayout();
        upRow->setSpacing(2);
        upRow->addStretch();
        auto *up = new KeyLabel("↑", this);
        m_keys.append({up, 56});
        keyMap[Qt::Key_Up].append(up);
        upRow->addWidget(up);
        upRow->addStretch();
        rightCol->addSpacing(4);
        rightCol->addLayout(upRow);

        auto *dnRow = new QHBoxLayout();
        dnRow->setSpacing(2);
        dnRow->addStretch();
        struct { Qt::Key c; QString t; } arr[3] = { {Qt::Key_Left,"←"},{Qt::Key_Down,"↓"},{Qt::Key_Right,"→"} };
        for (auto &a : arr) {
            auto *k = new KeyLabel(a.t, this);
            m_keys.append({k, 56});
            keyMap[a.c].append(k);
            dnRow->addWidget(k);
        }
        dnRow->addStretch();
        rightCol->addLayout(dnRow);
    }

    // Numpad
    QList<QList<KeyDef>> padRows = {
        { {Qt::Key_NumLock,"Num",46,true},{Qt::Key_Slash,"/",46,true},{Qt::Key_Asterisk,"*",46,true},{Qt::Key_Minus,"-",46,true} },
        { {Qt::Key_7,"7",46,true},{Qt::Key_8,"8",46,true},{Qt::Key_9,"9",46,true},{Qt::Key_Plus,"+",46,true} },
        { {Qt::Key_4,"4",46,true},{Qt::Key_5,"5",46,true},{Qt::Key_6,"6",46,true} },
        { {Qt::Key_1,"1",46,true},{Qt::Key_2,"2",46,true},{Qt::Key_3,"3",46,true},{Qt::Key_Enter,"Ent",46,true} },
        { {Qt::Key_0,"0",96,true},{Qt::Key_Period,".",46,true} },
    };
    rightCol->addSpacing(6);
    buildRows(rightCol, padRows);
    rightCol->addStretch();

    // Combine main + right into one row inside a framed panel (no trailing
    // stretch so the panel hugs its content and can be centered).
    auto *kbRow = new QHBoxLayout();
    kbRow->setSpacing(20);
    kbRow->addLayout(mainCol);
    kbRow->addLayout(rightCol);

    auto *panel = new QFrame();
    panel->setObjectName("KbPanel");
    panel->setStyleSheet("QFrame#KbPanel{background:#d9e2ea;"
                         " border:1px solid #b4c2ce; border-radius:12px;}");
    panel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 12, 14, 12);
    panelLayout->addLayout(kbRow);
    outer->addWidget(panel, 0, Qt::AlignHCenter);

    // ── Trackpad / mouse test (centered, same width feel) ─
    trackpadLabel = new TrackpadLabel(
        "Trackpad / Mouse Test Area\nMove cursor here, click buttons, scroll", this);
    trackpadLabel->setFixedHeight(100);
    trackpadLabel->setMinimumWidth(560);
    trackpadLabel->setMaximumWidth(900);
    outer->addWidget(trackpadLabel, 0, Qt::AlignHCenter);

    outer->addStretch();

    relayoutKeys(); // initial sizing
}

// Scales key sizes to the window width.
void InputTestWidget::relayoutKeys() {
    if (m_keys.isEmpty()) return;

    // Approximate full-keyboard logical width (widest main row + right cluster).
    const double neededLogical = 1010.0;
    const int avail = width() - 60; // outer + panel margins
    double scale = (avail > 0) ? avail / neededLogical : 0.8;
    scale = qBound(0.60, scale, 1.05);

    const int h = qRound(46 * scale);
    for (const auto &pr : m_keys)
        pr.first->setFixedSize(qRound(pr.second * scale), h);
}

// Re-layouts the keys.
void InputTestWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    relayoutKeys();
}

// Looks the key up (numpad flagged by KEYPAD_FLAG) and updates its state.
void InputTestWidget::handleKey(int key, bool pressed, bool keypad) {
    // Try the variant matching the keypad modifier first, then the other one,
    // so lock keys (NumLock) and keys whose modifier is inconsistent still light.
    const int primary   = keypad ? (key | KEYPAD_FLAG) : key;
    const int secondary = keypad ? key : (key | KEYPAD_FLAG);
    const int match = keyMap.contains(primary) ? primary
                    : (keyMap.contains(secondary) ? secondary : 0);
    if (match)
        for (auto *lbl : keyMap[match])
            lbl->setActive(pressed);
}

// Some keys (esp. CapsLock / NumLock / ScrollLock) can arrive with an unreliable
// Qt key code; fall back to the Windows virtual-key code to identify them.
static int resolveKey(QKeyEvent *event) {
    switch (event->nativeVirtualKey()) {
        case 0x14: return Qt::Key_CapsLock;   // VK_CAPITAL
        case 0x90: return Qt::Key_NumLock;    // VK_NUMLOCK
        case 0x91: return Qt::Key_ScrollLock; // VK_SCROLL
        default:   return event->key();
    }
}

// Forwards to handleKey(pressed).
void InputTestWidget::keyPressEvent(QKeyEvent *event) {
    handleKey(resolveKey(event), true, event->modifiers() & Qt::KeypadModifier);
    event->accept();
}

// Forwards to handleKey(released).
void InputTestWidget::keyReleaseEvent(QKeyEvent *event) {
    handleKey(resolveKey(event), false, event->modifiers() & Qt::KeypadModifier);
    event->accept();
}

// Returns false so Tab is not used for focus navigation and can be tested.
bool InputTestWidget::focusNextPrevChild(bool next) {
    Q_UNUSED(next);
    return false; // keep Tab inside the widget so it can be tested
}
