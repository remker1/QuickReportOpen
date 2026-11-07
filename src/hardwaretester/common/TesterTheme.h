// =============================================================================
// TesterTheme.h - colour tokens for custom-painted tester widgets.
// =============================================================================
#ifndef TESTERTHEME_H
#define TESTERTHEME_H

#include <QColor>

// Central color tokens for all custom-painted tester widgets. Widgets must not
// hard-code hex values so a dark set can be added later without touching them.
namespace TesterTheme {

// One colour set for the custom-painted widgets.
struct Palette {
    QColor accent;          // interactive highlight (matches app QSS accent)
    QColor accentSoft;      // "tested" state, light accent wash
    QColor cardBg;
    QColor cardBorder;
    QColor textPrimary;
    QColor textSecondary;
    QColor canvasBg;        // drawing canvases (trail, waveform, chart)
    QColor gridLine;
    QColor ok;
    QColor warn;
    QColor error;           // chatter keys, throttle flag
    QColor series1;         // chart line colors
    QColor series2;
    QColor series3;
};

// The light palette (accent #5C7C98 matches the app style sheet).
inline const Palette &light() {
    static const Palette p{
        QColor("#5C7C98"),  // accent — same as app QSS
        QColor("#D5E2ED"),
        QColor("#FFFFFF"),
        QColor("#D8DEE4"),
        QColor("#222933"),
        QColor("#6B7683"),
        QColor("#F4F6F8"),
        QColor("#E3E8ED"),
        QColor("#2E9E5B"),
        QColor("#D98E04"),
        QColor("#C0392B"),
        QColor("#5C7C98"),
        QColor("#D98E04"),
        QColor("#C0392B"),
    };
    return p;
}

// Active palette. Later: switch on app theme; today the app is light-only.
inline const Palette &current() { return light(); }

} // namespace TesterTheme

#endif // TESTERTHEME_H
