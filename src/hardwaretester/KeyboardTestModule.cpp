// =============================================================================
// KeyboardTestModule.cpp - keyboard test page (layouts, Raw Input handling, chatter).
// =============================================================================
#include "KeyboardTestModule.h"
#include "common/Card.h"
#include "common/RawInputManager.h"
#include "common/TesterTheme.h"

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Physical key identity: Set-1 make code | e0<<8 | e1<<9.
constexpr quint32 E0 = 0x100;
constexpr quint32 E1 = 0x200;

// Definition of one key cap: label, key code and logical width.
struct KeyDef {
    quint32 code;
    const char *label;
    int width; // logical width at base key size 46
};

// Main block rows (function row .. bottom row).
// Main block rows of the English (ANSI) layout.
const QList<QList<KeyDef>> &mainRows() {
    static const QList<QList<KeyDef>> rows = {
        { {0x01,"Esc",46}, {0x3B,"F1",46},{0x3C,"F2",46},{0x3D,"F3",46},{0x3E,"F4",46},
          {0x3F,"F5",46},{0x40,"F6",46},{0x41,"F7",46},{0x42,"F8",46},
          {0x43,"F9",46},{0x44,"F10",46},{0x57,"F11",46},{0x58,"F12",46} },
        { {0x29,"`",46},{0x02,"1",46},{0x03,"2",46},{0x04,"3",46},{0x05,"4",46},
          {0x06,"5",46},{0x07,"6",46},{0x08,"7",46},{0x09,"8",46},{0x0A,"9",46},
          {0x0B,"0",46},{0x0C,"-",46},{0x0D,"=",46},{0x0E,"Bksp",76} },
        { {0x0F,"Tab",70},{0x10,"Q",46},{0x11,"W",46},{0x12,"E",46},{0x13,"R",46},
          {0x14,"T",46},{0x15,"Y",46},{0x16,"U",46},{0x17,"I",46},{0x18,"O",46},
          {0x19,"P",46},{0x1A,"[",46},{0x1B,"]",46},{0x2B,"\\",52} },
        { {0x3A,"Caps",84},{0x1E,"A",46},{0x1F,"S",46},{0x20,"D",46},{0x21,"F",46},
          {0x22,"G",46},{0x23,"H",46},{0x24,"J",46},{0x25,"K",46},{0x26,"L",46},
          {0x27,";",46},{0x28,"'",46},{0x1C,"Enter",88} },
        { {0x2A,"Shift",108},{0x2C,"Z",46},{0x2D,"X",46},{0x2E,"C",46},{0x2F,"V",46},
          {0x30,"B",46},{0x31,"N",46},{0x32,"M",46},{0x33,",",46},{0x34,".",46},
          {0x35,"/",46},{0x36,"Shift",112} },
        { {0x1D,"Ctrl",64},{0x5B|E0,"Win",56},{0x38,"Alt",56},{0x39,"Space",300},
          {0x38|E0,"Alt",56},{0x5C|E0,"Win",56},{0x5D|E0,"Menu",56},{0x1D|E0,"Ctrl",64} },
    };
    return rows;
}

// Canadian French — matches the Lenovo ThinkPad Canadian Multilingual Standard
// (CSA) keyboard. Same physical scan codes as the English layout, so Raw Input
// still matches by code; only the key set differs (ISO adds 0x56 next to
// Left-Shift and moves 0x2B beside a shorter Enter) and the legends show the
// French/CSA keycaps and the ThinkPad modifier row (Ctrl/Win/Alt/AltGr/Ctrl).
// Main block rows of the Canadian-French (Apple ISO) layout.
const QList<QList<KeyDef>> &frenchMainRows() {
    static const QList<QList<KeyDef>> rows = {
        { {0x01,"Esc",46}, {0x3B,"F1",46},{0x3C,"F2",46},{0x3D,"F3",46},{0x3E,"F4",46},
          {0x3F,"F5",46},{0x40,"F6",46},{0x41,"F7",46},{0x42,"F8",46},
          {0x43,"F9",46},{0x44,"F10",46},{0x57,"F11",46},{0x58,"F12",46} },
        { {0x29,"#",46},{0x02,"1",46},{0x03,"2",46},{0x04,"3",46},{0x05,"4",46},
          {0x06,"5",46},{0x07,"6",46},{0x08,"7",46},{0x09,"8",46},{0x0A,"9",46},
          {0x0B,"0",46},{0x0C,"-",46},{0x0D,"=",46},{0x0E,"Bksp",76} },
        { {0x0F,"Tab",70},{0x10,"Q",46},{0x11,"W",46},{0x12,"E",46},{0x13,"R",46},
          {0x14,"T",46},{0x15,"Y",46},{0x16,"U",46},{0x17,"I",46},{0x18,"O",46},
          {0x19,"P",46},{0x1A,"^",46},{0x1B,"ç",46} },
        { {0x3A,"Caps",84},{0x1E,"A",46},{0x1F,"S",46},{0x20,"D",46},{0x21,"F",46},
          {0x22,"G",46},{0x23,"H",46},{0x24,"J",46},{0x25,"K",46},{0x26,"L",46},
          {0x27,";",46},{0x28,"`",46},{0x2B,"\\",46},{0x1C,"Enter",70} },
        { {0x2A,"Shift",76},{0x56,"«",46},{0x2C,"Z",46},{0x2D,"X",46},{0x2E,"C",46},
          {0x2F,"V",46},{0x30,"B",46},{0x31,"N",46},{0x32,"M",46},{0x33,",",46},
          {0x34,".",46},{0x35,"É",46},{0x36,"Shift",112} },
        // PrtSc lives in the nav cluster (shared with the English layout); a
        // second copy here would collide on scan code 0x37|E0, so the bottom
        // row keeps just the ThinkPad modifiers.
        { {0x1D,"Ctrl",64},{0x5B|E0,"Win",56},{0x38,"Alt",56},{0x39,"Space",320},
          {0x38|E0,"AltGr",64},{0x1D|E0,"Ctrl",64} },
    };
    return rows;
}

// Right cluster: PrtSc row + Ins/Del blocks. Pause is E1-prefixed; Raw Input
// reports it as make code 0x1D with the E1 flag (the trailing fake 0x45
// stroke carries VKey 255 and is dropped by RawInputManager).
// Navigation cluster (insert/home/arrows ...).
const QList<QList<KeyDef>> &navRows() {
    static const QList<QList<KeyDef>> rows = {
        { {0x37|E0,"PrtSc",56},{0x46,"ScrLk",56},{0x1D|E1,"Pause",56} },
        { {0x52|E0,"Ins",56},{0x47|E0,"Home",56},{0x49|E0,"PgUp",56} },
        { {0x53|E0,"Del",56},{0x4F|E0,"End",56},{0x51|E0,"PgDn",56} },
    };
    return rows;
}

// Numeric keypad rows.
const QList<QList<KeyDef>> &padRows() {
    static const QList<QList<KeyDef>> rows = {
        { {0x45,"Num",46},{0x35|E0,"/",46},{0x37,"*",46},{0x4A,"-",46} },
        { {0x47,"7",46},{0x48,"8",46},{0x49,"9",46},{0x4E,"+",46} },
        { {0x4B,"4",46},{0x4C,"5",46},{0x4D,"6",46} },
        { {0x4F,"1",46},{0x50,"2",46},{0x51,"3",46},{0x1C|E0,"Ent",46} },
        { {0x52,"0",96},{0x53,".",46} },
    };
    return rows;
}

// Builds the style sheet for a key cap from three colours.
QString styleFor(const QColor &bg, const QColor &fg, const QColor &border) {
    return QString("QLabel{background:%1;color:%2;border:1px solid %3;"
                   "border-radius:5px;font-size:11px;font-weight:600;}")
        .arg(bg.name(), fg.name(), border.name());
}

} // namespace

// ── TesterKeyLabel ─────────────────────────────────────────────

TesterKeyLabel::TesterKeyLabel(const QString &text, QWidget *parent)
    : QLabel(text, parent) {
    setAlignment(Qt::AlignCenter);
    applyStyle();
}

// (see header)
void TesterKeyLabel::setPressed(bool down) {
    m_pressed = down;
    if (down)
        m_visited = true;
    applyStyle();
}

// (see header)
void TesterKeyLabel::setChattered() {
    m_chattered = true;
    applyStyle();
}

// (see header)
void TesterKeyLabel::reset() {
    m_pressed = m_visited = m_chattered = false;
    applyStyle();
}

// (see header) Colours: idle, pressed (accent), visited (soft accent), chattered (error).
void TesterKeyLabel::applyStyle() {
    const auto &p = TesterTheme::current();
    if (m_pressed)
        setStyleSheet(styleFor(p.accent, Qt::black, p.accent.darker(140)));
    else if (m_chattered)
        setStyleSheet(styleFor(p.error.lighter(175), p.error.darker(120), p.error));
    else if (m_visited)
        setStyleSheet(styleFor(p.accentSoft, Qt::black, p.accent.lighter(115)));
    else
        setStyleSheet(styleFor(p.cardBg, Qt::black, p.cardBorder));
}

// ── KeyboardTestModule ─────────────────────────────────────────

KeyboardTestModule::KeyboardTestModule(Translator t, QWidget *parent)
    : TesterModule(std::move(t), parent) {
    m_clock.start();

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 10, 14, 10);
    outer->setSpacing(8);

    // Top bar: counters + numpad toggle + reset.
    auto *top = new QHBoxLayout();
    m_countLabel = new QLabel(this);
    m_nkroLabel = new QLabel(this);
    m_hideNumpad = new QCheckBox(this);
    m_frenchLayout = new QCheckBox(this);
    m_resetBtn = new QPushButton(this);
    // The whole point of this module is to press every key, including Space and
    // Enter. If these controls could take keyboard focus, Space/Enter would
    // "click" the focused one (Qt activates the focused button/checkbox on those
    // keys) and silently reset the test. Raw Input still feeds the on-screen
    // visualization regardless of focus, so denying focus loses nothing.
    m_hideNumpad->setFocusPolicy(Qt::NoFocus);
    m_frenchLayout->setFocusPolicy(Qt::NoFocus);
    m_resetBtn->setFocusPolicy(Qt::NoFocus);
    m_resetBtn->setAutoDefault(false);
    m_resetBtn->setDefault(false);
    top->addWidget(m_countLabel);
    top->addSpacing(24);
    top->addWidget(m_nkroLabel);
    top->addStretch();
    top->addWidget(m_frenchLayout);
    top->addSpacing(12);
    top->addWidget(m_hideNumpad);
    top->addSpacing(12);
    top->addWidget(m_resetBtn);
    outer->addLayout(top);

    // Keyboard panel — the key grid inside is rebuilt in place whenever the
    // layout switches between English (ANSI) and Canadian French (Apple ISO).
    m_kbPanel = new QFrame(this);
    m_kbPanel->setObjectName("KbPanel");
    m_kbPanel->setStyleSheet("QFrame#KbPanel{background:#d9e2ea;"
                             " border:1px solid #b4c2ce; border-radius:12px;}");
    m_kbPanel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    m_kbPanelLayout = new QVBoxLayout(m_kbPanel);
    m_kbPanelLayout->setContentsMargins(14, 12, 14, 12);
    outer->addWidget(m_kbPanel, 0, Qt::AlignHCenter);
    buildKeyboardGrid();

    // Info cards: last key / history / chatter.
    auto *cards = new QHBoxLayout();
    cards->setSpacing(10);

    m_lastKeyCard = new Card(QString(), this);
    m_lastKeyLabel = new QLabel(this);
    m_lastKeyLabel->setTextFormat(Qt::RichText);
    m_lastKeyCard->body()->addWidget(m_lastKeyLabel);
    m_lastKeyCard->body()->addStretch();
    cards->addWidget(m_lastKeyCard, 1);

    m_historyCard = new Card(QString(), this);
    m_history = new QListWidget(this);
    m_history->setFrameShape(QFrame::NoFrame);
    m_history->setFocusPolicy(Qt::NoFocus);
    m_history->setSelectionMode(QAbstractItemView::NoSelection);
    m_historyCard->body()->addWidget(m_history);
    cards->addWidget(m_historyCard, 1);

    m_chatterCard = new Card(QString(), this);
    m_chatterLabel = new QLabel(this);
    m_chatterLabel->setWordWrap(true);
    m_chatterCard->body()->addWidget(m_chatterLabel);
    m_chatterCard->body()->addStretch();
    cards->addWidget(m_chatterCard, 1);

    outer->addLayout(cards, 1);

    m_fnNote = new QLabel(this);
    m_fnNote->setWordWrap(true);
    m_fnNote->setStyleSheet(QString("color:%1;font-size:11px;")
                                .arg(TesterTheme::current().textSecondary.name()));
    outer->addWidget(m_fnNote);

    connect(m_resetBtn, &QPushButton::clicked, this, &KeyboardTestModule::onReset);
    connect(m_hideNumpad, &QCheckBox::toggled, this, [this](bool hide) {
        m_numpadPanel->setVisible(!hide);
        updateCounters();
    });
    connect(m_frenchLayout, &QCheckBox::toggled, this, [this](bool fr) {
        m_french = fr;
        buildKeyboardGrid(); // swaps the whole grid (ANSI ↔ Apple ISO)
        onReset();           // fresh keys start untested
    });
    connect(&RawInputManager::instance(), &RawInputManager::keyEvent, this,
            &KeyboardTestModule::onKeyEvent);

    // Default the numpad hidden on physically small screens (< 15"), where the
    // full 104-key layout gets cramped and the right-hand clusters can clip;
    // show the full keyboard on 15" and larger. Falls back to full keyboard
    // when the display doesn't report a usable physical size.
    bool hideNumpadDefault = false;
    if (QScreen *scr = QGuiApplication::primaryScreen()) {
        const QSizeF mm = scr->physicalSize();
        const double diagIn = std::hypot(mm.width(), mm.height()) / 25.4;
        if (diagIn > 1.0 && diagIn < 15.0) // > 1" guards against bogus/zero EDID
            hideNumpadDefault = true;
    }
    m_hideNumpad->setChecked(hideNumpadDefault); // fires toggled → hides panel

    retranslate();
    updateCounters();
}

// (see header) Creates a key cap for every KeyDef of the active layout.
void KeyboardTestModule::buildKeyboardGrid() {
    // Tear down the previous grid; deleting the host frees every key widget it
    // owns. Called from the GUI thread only (constructor / French toggle), so a
    // synchronous delete is safe.
    if (m_kbHost) {
        m_kbPanelLayout->removeWidget(m_kbHost);
        delete m_kbHost;
        m_kbHost = nullptr;
    }
    m_keyWidgets.clear();
    m_scalable.clear();
    m_numpadCodes.clear();

    m_kbHost = new QWidget(m_kbPanel);
    auto *kbRow = new QHBoxLayout(m_kbHost);
    kbRow->setContentsMargins(0, 0, 0, 0);
    kbRow->setSpacing(20);

    auto buildRows = [this](QVBoxLayout *dst, const QList<QList<KeyDef>> &rows) {
        for (const auto &row : rows) {
            auto *rl = new QHBoxLayout();
            rl->setSpacing(2);
            for (const KeyDef &kd : row) {
                auto *k = new TesterKeyLabel(QString::fromUtf8(kd.label), m_kbHost);
                m_keyWidgets.insert(kd.code, k);
                m_scalable.append({k, kd.width});
                rl->addWidget(k);
            }
            rl->addStretch();
            dst->addLayout(rl);
        }
    };

    auto *mainCol = new QVBoxLayout();
    mainCol->setSpacing(3);
    buildRows(mainCol, m_french ? frenchMainRows() : mainRows());

    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(3);
    buildRows(rightCol, navRows());

    // Arrow cluster (inverted-T).
    {
        auto *upRow = new QHBoxLayout();
        upRow->setSpacing(2);
        upRow->addStretch();
        auto *up = new TesterKeyLabel(QString::fromUtf8("↑"), m_kbHost);
        m_keyWidgets.insert(0x48 | E0, up);
        m_scalable.append({up, 56});
        upRow->addWidget(up);
        upRow->addStretch();
        rightCol->addSpacing(4);
        rightCol->addLayout(upRow);

        auto *dnRow = new QHBoxLayout();
        dnRow->setSpacing(2);
        dnRow->addStretch();
        const struct { quint32 c; const char *t; } arr[3] = {
            {0x4B | E0, "←"}, {0x50 | E0, "↓"}, {0x4D | E0, "→"}};
        for (const auto &a : arr) {
            auto *k = new TesterKeyLabel(QString::fromUtf8(a.t), m_kbHost);
            m_keyWidgets.insert(a.c, k);
            m_scalable.append({k, 56});
            dnRow->addWidget(k);
        }
        dnRow->addStretch();
        rightCol->addLayout(dnRow);
    }

    // Numpad in its own container so it can be hidden.
    m_numpadPanel = new QWidget(m_kbHost);
    auto *padCol = new QVBoxLayout(m_numpadPanel);
    padCol->setContentsMargins(0, 0, 0, 0);
    padCol->setSpacing(3);
    {
        auto *tmp = new QVBoxLayout();
        tmp->setSpacing(3);
        buildRows(tmp, padRows());
        padCol->addLayout(tmp);
    }
    for (const auto &row : padRows())
        for (const KeyDef &kd : row)
            m_numpadCodes.insert(kd.code);

    kbRow->addLayout(mainCol);
    kbRow->addLayout(rightCol);
    kbRow->addWidget(m_numpadPanel);

    m_kbPanelLayout->addWidget(m_kbHost);

    // Re-apply the current numpad-visibility choice to the freshly built panel.
    if (m_hideNumpad)
        m_numpadPanel->setVisible(!m_hideNumpad->isChecked());
    relayoutKeys();
}

// Acquires Raw Input (keyboard), installs the key-swallowing event filter.
void KeyboardTestModule::moduleEntered() {
    RawInputManager::instance().acquireKeyboard();
    // Swallow Qt key events app-wide while testing so navigation/scroll keys
    // (arrows, PgUp/PgDn, Home/End, Esc, Tab, Space, Enter...) can't move the
    // module list, scroll the view, or fire shortcuts. Raw Input (WM_INPUT)
    // drives the on-screen keyboard independently, so nothing visible is lost.
    qApp->installEventFilter(this);
}

// Releases Raw Input and removes the event filter.
void KeyboardTestModule::moduleLeft() {
    qApp->removeEventFilter(this);
    RawInputManager::instance().releaseKeyboard();
}

// (see header) Swallows key events while testing.
bool KeyboardTestModule::eventFilter(QObject *obj, QEvent *event) {
    switch (event->type()) {
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
            return true; // consume — do not deliver to any widget
        case QEvent::ShortcutOverride:
            // Accept so the key is treated as a normal (swallowed) keypress
            // instead of triggering an application shortcut.
            event->accept();
            return true;
        default:
            return TesterModule::eventFilter(obj, event);
    }
}

// (see header)
void KeyboardTestModule::onKeyEvent(quint32 code, quint16 vkey, bool pressed) {
    if (!isVisible())
        return;
    auto *widget = m_keyWidgets.value(code, nullptr);
    const qint64 now = m_clock.nsecsElapsed() / 1000000;

    if (pressed) {
        if (m_down.contains(code))
            return; // typematic auto-repeat, not a new press
        // Chatter: a "new" press unrealistically soon after the release of
        // the same key is a bouncing switch, not a human double-press.
        if (m_lastRelease.contains(code) && now - m_lastRelease.value(code) < kChatterMs) {
            m_chatterCount[code]++;
            if (widget)
                widget->setChattered();
        }
        m_down.insert(code);
        m_visited.insert(code);
        m_maxSimultaneous = qMax(m_maxSimultaneous, int(m_down.size()));

        // History (most recent first, capped at 12).
        m_history->insertItem(0, keyName(code));
        while (m_history->count() > 12)
            delete m_history->takeItem(m_history->count() - 1);

        m_lastKeyLabel->setText(
            QString("<b style='font-size:16px'>%1</b><br>%2: 0x%3%4<br>%5: 0x%6 (%7)")
                .arg(keyName(code), tr_("ht_kb_scancode"),
                     QString::number(code & 0xFF, 16).toUpper().rightJustified(2, '0'),
                     (code & E0) ? " E0" : (code & E1) ? " E1" : "",
                     tr_("ht_kb_vk"),
                     QString::number(vkey, 16).toUpper().rightJustified(2, '0'))
                .arg(vkey));
    } else {
        m_down.remove(code);
        m_lastRelease[code] = now;
    }

    if (widget)
        widget->setPressed(pressed);
    updateCounters();
}

// (see header)
QString KeyboardTestModule::keyName(quint32 code) const {
    auto *w = m_keyWidgets.value(code, nullptr);
    QString base = w ? w->text() : QString("sc 0x%1").arg(code, 0, 16);
    if (m_numpadCodes.contains(code))
        base = "Num " + base;
    // Distinguish the paired modifiers in text (widgets already do visually).
    if (code == 0x2A) base = "L-Shift";
    if (code == 0x36) base = "R-Shift";
    if (code == 0x1D) base = "L-Ctrl";
    if (code == (0x1D | E0)) base = "R-Ctrl";
    if (code == 0x38) base = "L-Alt";
    if (code == (0x38 | E0)) base = "R-Alt";
    if (code == (0x5B | E0)) base = "L-Win";
    if (code == (0x5C | E0)) base = "R-Win";
    return base;
}

// (see header)
void KeyboardTestModule::updateCounters() {
    int total = 0, tested = 0;
    const bool numpadHidden = m_hideNumpad->isChecked();
    for (auto it = m_keyWidgets.constBegin(); it != m_keyWidgets.constEnd(); ++it) {
        if (numpadHidden && m_numpadCodes.contains(it.key()))
            continue;
        ++total;
        if (m_visited.contains(it.key()))
            ++tested;
    }
    m_countLabel->setText(tr_("ht_kb_tested").replace("{tested}", QString::number(tested))
                              .replace("{total}", QString::number(total)));
    m_nkroLabel->setText(tr_("ht_kb_nkro").replace("{now}", QString::number(m_down.size()))
                             .replace("{max}", QString::number(m_maxSimultaneous)));

    int chatterTotal = 0;
    QStringList chatterKeys;
    for (auto it = m_chatterCount.constBegin(); it != m_chatterCount.constEnd(); ++it) {
        chatterTotal += it.value();
        chatterKeys << QString("%1 ×%2").arg(keyName(it.key())).arg(it.value());
    }
    m_chatterLabel->setText(chatterTotal == 0
        ? tr_("ht_kb_chatter_none")
        : tr_("ht_kb_chatter_found").replace("{n}", QString::number(chatterTotal))
              + "\n" + chatterKeys.join(", "));

    recordResult("ht_kb_result_name",
                 QString("%1/%2, %3 chatter").arg(tested).arg(total).arg(chatterTotal));
}

// (see header)
void KeyboardTestModule::onReset() {
    m_down.clear();
    m_visited.clear();
    m_lastRelease.clear();
    m_chatterCount.clear();
    m_maxSimultaneous = 0;
    m_history->clear();
    m_lastKeyLabel->setText(tr_("ht_kb_press_any"));
    for (auto *w : m_keyWidgets)
        w->reset();
    updateCounters();
}

// (see header)
void KeyboardTestModule::relayoutKeys() {
    if (m_scalable.isEmpty())
        return;
    const double neededLogical = 1010.0;
    const int avail = width() - 60;
    double scale = (avail > 0) ? avail / neededLogical : 0.8;
    scale = qBound(0.50, scale, 1.0);
    const int h = qRound(40 * scale);
    for (const auto &pr : m_scalable)
        pr.first->setFixedSize(qRound(pr.second * scale), h);
}

// Re-scales the key caps whenever the page size changes.
void KeyboardTestModule::resizeEvent(QResizeEvent *event) {
    TesterModule::resizeEvent(event);
    relayoutKeys();
}

// Re-applies translated texts.
void KeyboardTestModule::retranslate() {
    m_resetBtn->setText(tr_("ht_common_reset"));
    m_hideNumpad->setText(tr_("ht_kb_hide_numpad"));
    m_frenchLayout->setText(tr_("ht_kb_french_layout"));
    m_lastKeyCard->setTitle(tr_("ht_kb_last_key"));
    m_historyCard->setTitle(tr_("ht_kb_history"));
    m_chatterCard->setTitle(tr_("ht_kb_chatter_title"));
    m_fnNote->setText(tr_("ht_kb_fn_note"));
    if (m_history->count() == 0)
        m_lastKeyLabel->setText(tr_("ht_kb_press_any"));
    updateCounters();
}
