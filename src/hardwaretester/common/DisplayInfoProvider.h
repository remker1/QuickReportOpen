// =============================================================================
// DisplayInfoProvider.h - per-monitor details (refresh rate, HDR, EDID) from Windows.
// =============================================================================
#ifndef DISPLAYINFOPROVIDER_H
#define DISPLAYINFOPROVIDER_H

#include <QList>
#include <QString>

// Per-monitor facts read straight from the OS — no estimates. Fields that
// could not be read stay empty/negative so the UI can show an honest
// "unavailable" instead of a guess.
struct MonitorInfo {
    QString friendlyName;   // e.g. "DELL U2723QE" (target device name)
    QString gdiDeviceName;  // e.g. "\\.\DISPLAY1" — matches QScreen::name()
    int pixelWidth = 0;     // active source mode (physical pixels)
    int pixelHeight = 0;
    double refreshHz = -1;  // vSync Numerator/Denominator, e.g. 59.951
    bool hdrSupported = false;
    bool hdrEnabled = false;
    int bitsPerChannel = 0; // 0 = unknown
    QString colorEncoding;  // RGB / YCbCr444 / ...
    QString edidVendor;     // 3-letter PnP id, empty if EDID unreadable
    QString edidModel;      // descriptor 0xFC string, empty if unreadable
};

namespace DisplayInfoProvider {
// Enumerates active display paths via QueryDisplayConfig. Returns an empty
// list only if the API itself fails.
QList<MonitorInfo> queryMonitors();
}

#endif // DISPLAYINFOPROVIDER_H
