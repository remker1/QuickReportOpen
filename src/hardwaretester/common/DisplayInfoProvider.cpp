// =============================================================================
// DisplayInfoProvider.cpp - monitor info via the Windows display-config API.
// =============================================================================
#include "DisplayInfoProvider.h"

#include <QDebug>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>

#include <vector>

namespace {

// {E6F07B5F-EE97-4a90-B076-33F57BF4EAA7}
const GUID kGuidDevInterfaceMonitor =
    {0xe6f07b5f, 0xee97, 0x4a90, {0xb0, 0x76, 0x33, 0xf5, 0x7b, 0xf4, 0xea, 0xa7}};

// Reads the EDID block for the monitor whose device-interface path matches
// `monitorDevicePath` (as reported by DISPLAYCONFIG_TARGET_DEVICE_NAME).
QByteArray readEdid(const QString &monitorDevicePath) {
    if (monitorDevicePath.isEmpty())
        return {};

    HDEVINFO devInfo = SetupDiGetClassDevsW(&kGuidDevInterfaceMonitor, nullptr, nullptr,
                                            DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (devInfo == INVALID_HANDLE_VALUE)
        return {};

    QByteArray edid;
    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &kGuidDevInterfaceMonitor,
                                                  i, &ifData); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &needed, nullptr);
        if (!needed)
            continue;
        std::vector<char> buf(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devData = {};
        devData.cbSize = sizeof(devData);
        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, nullptr,
                                              &devData))
            continue;
        if (QString::fromWCharArray(detail->DevicePath)
                .compare(monitorDevicePath, Qt::CaseInsensitive) != 0)
            continue;

        HKEY key = SetupDiOpenDevRegKey(devInfo, &devData, DICS_FLAG_GLOBAL, 0,
                                        DIREG_DEV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            BYTE data[1024];
            DWORD size = sizeof(data), type = 0;
            if (RegQueryValueExW(key, L"EDID", nullptr, &type, data, &size) == ERROR_SUCCESS &&
                size >= 128)
                edid = QByteArray(reinterpret_cast<char *>(data), int(size));
            RegCloseKey(key);
        }
        break;
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    return edid;
}

void parseEdid(const QByteArray &edid, MonitorInfo &info) {
    if (edid.size() < 128)
        return;
    const auto *b = reinterpret_cast<const unsigned char *>(edid.constData());

    // Manufacturer: 2 bytes at offset 8, three 5-bit letters ('A' - 1 based).
    const quint16 mfg = quint16(b[8]) << 8 | b[9];
    char v[4] = {char('A' - 1 + ((mfg >> 10) & 0x1F)),
                 char('A' - 1 + ((mfg >> 5) & 0x1F)),
                 char('A' - 1 + (mfg & 0x1F)), 0};
    if (v[0] >= 'A' && v[0] <= 'Z')
        info.edidVendor = QString::fromLatin1(v);

    // Descriptor blocks: 4 x 18 bytes at 54; tag 0xFC = monitor name.
    for (int d = 0; d < 4; ++d) {
        const unsigned char *desc = b + 54 + d * 18;
        if (desc[0] == 0 && desc[1] == 0 && desc[3] == 0xFC) {
            QByteArray name(reinterpret_cast<const char *>(desc + 5), 13);
            const int nl = name.indexOf('\n');
            if (nl >= 0)
                name.truncate(nl);
            info.edidModel = QString::fromLatin1(name).trimmed();
        }
    }
}

QString encodingName(DISPLAYCONFIG_COLOR_ENCODING enc) {
    switch (enc) {
        case DISPLAYCONFIG_COLOR_ENCODING_RGB: return "RGB";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444: return "YCbCr 4:4:4";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422: return "YCbCr 4:2:2";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420: return "YCbCr 4:2:0";
        case DISPLAYCONFIG_COLOR_ENCODING_INTENSITY: return "Intensity";
        default: return {};
    }
}

} // namespace

// Uses QueryDisplayConfig / DisplayConfigGetDeviceInfo to collect, for each active
// path: friendly name, GDI name, mode size, exact refresh (numerator/denominator),
// HDR support/state, bits per channel, colour encoding and EDID data.
QList<MonitorInfo> DisplayInfoProvider::queryMonitors() {
    QList<MonitorInfo> monitors;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
        ERROR_SUCCESS)
        return monitors;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                           modes.data(), nullptr) != ERROR_SUCCESS)
        return monitors;
    paths.resize(pathCount);

    for (const DISPLAYCONFIG_PATH_INFO &path : paths) {
        MonitorInfo info;

        // Precise refresh rate straight from the path's vertical sync ratio.
        const auto &vs = path.targetInfo.refreshRate;
        if (vs.Denominator != 0)
            info.refreshHz = double(vs.Numerator) / double(vs.Denominator);

        // Active source mode = physical pixel resolution.
        const UINT32 srcModeIdx = path.sourceInfo.modeInfoIdx;
        if (srcModeIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && srcModeIdx < modeCount &&
            modes[srcModeIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            info.pixelWidth = int(modes[srcModeIdx].sourceMode.width);
            info.pixelHeight = int(modes[srcModeIdx].sourceMode.height);
        }

        // GDI device name ("\\.\DISPLAY1") — lets callers match QScreen.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName = {};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = path.sourceInfo.adapterId;
        srcName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) == ERROR_SUCCESS)
            info.gdiDeviceName = QString::fromWCharArray(srcName.viewGdiDeviceName);

        // Friendly monitor name + device path (for EDID lookup).
        DISPLAYCONFIG_TARGET_DEVICE_NAME tgtName = {};
        tgtName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tgtName.header.size = sizeof(tgtName);
        tgtName.header.adapterId = path.targetInfo.adapterId;
        tgtName.header.id = path.targetInfo.id;
        QString monitorDevicePath;
        if (DisplayConfigGetDeviceInfo(&tgtName.header) == ERROR_SUCCESS) {
            if (tgtName.flags.friendlyNameFromEdid)
                info.friendlyName = QString::fromWCharArray(tgtName.monitorFriendlyDeviceName);
            monitorDevicePath = QString::fromWCharArray(tgtName.monitorDevicePath);
        }

        // HDR / advanced color state.
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color = {};
        color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        color.header.size = sizeof(color);
        color.header.adapterId = path.targetInfo.adapterId;
        color.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
            info.hdrSupported = color.advancedColorSupported;
            info.hdrEnabled = color.advancedColorEnabled;
            info.bitsPerChannel = int(color.bitsPerColorChannel);
            info.colorEncoding = encodingName(color.colorEncoding);
        }

        parseEdid(readEdid(monitorDevicePath), info);
        monitors.append(info);
    }
    return monitors;
}
