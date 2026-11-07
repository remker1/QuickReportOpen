## QuickReports

QuickReports is a Windows Qt 6 (C++/Widgets) desktop app that shows a system summary (storage / motherboard / BIOS / battery) and provides a set of admin-required utility actions plus an in-depth hardware tester. Several actions require administrator privileges; the app attempts to relaunch elevated at startup.

## Features

### System information

- System scan and summary UI (System Summary tab)
- Storage details tree with per-disk / per-partition context-menu actions:
  - Clear a target drive (`diskpart clean`)
  - Initialize a disk and create/format a volume
  - Extend a volume into adjacent unallocated space
  - Add adjacent unallocated space to the system drive
  - Clone the system to a target disk (full clone or used-space-only clone)
- Motherboard & BIOS details
- Hardware activation status display (Windows + Office)
- Multilingual UI: English (`en`), French (`fr`), Simplified Chinese (`zh`)

### Utility actions (admin-required)

- **Install Softwares** — pick an installer (`.iso` / `.img` / `.exe`) from the `Softwares` folder next to the launcher; images are mounted and `Setup.exe` is run if present
- **Open Windows Update** — with a badge that appears when updates (including driver updates) are pending
- **Auto Time Zone & Sync** — enables location + automatic time zone and forces an immediate time resync
- **Create Office Folder** — sets up an Office working folder
- **Open Word Account** — opens the Word account page (shows a friendly message if Word is not installed)
- **Turn Off Device Encryption**
- Copy the portable app to the Desktop, or delete it when already run from the Desktop

### Hardware Tester (deep test modules)

The Hardware Test button opens a dedicated window hosting accuracy-first test modules (left-side navigation):

1. **Keyboard** — full 104-key layout driven by Raw Input (`WM_INPUT`), so PrtSc/Pause/Win register; scan code / virtual key panel, 12-key history, NKRO counter, and switch-chatter detection (<35 ms re-trigger)
2. **Mouse** — 5-button diagram (L/R/M/X1/X2), double-click interval vs the system threshold, 4-direction wheel counters, movement trail, and a **real polling-rate measurement** from Raw Input event timestamps (125/500/1000 Hz+)
3. **Trackpad / Touch** — multi-touch and gesture reporting
4. **Camera** — Qt Multimedia device/format enumeration, live preview with mirror, nominal vs **measured** fps, PNG snapshots, and distinct no-device / permission-denied / device-busy states
5. **Speaker & Mic** — sine test tone with L/R/both channel gating and a log-scale 100 Hz–20 kHz slider + presets; mic RMS level bar, waveform, 5 s record & playback
6. **Display** — per-monitor **precise refresh rate** (QueryDisplayConfig numerator/denominator, e.g. 59.951), HDR state, bits per channel, EDID panel id, and a fullscreen fill test (solid colors + grayscale gradient) per monitor
7. **Fan** — fan RPM metrics

Anything the OS cannot report (e.g. an unreadable EDID) is shown as "not supported" — never estimated.

## Prerequisites

- Windows 10/11
- Qt 6 (tested/configured for `C:/Qt/6.10.2/msvc2022_64`)
- CMake >= 3.16
- Visual Studio 2022 (MSVC) + C++ build tools
- `windeployqt` (used to package Qt runtime DLLs)
- Admin privileges are required for several actions; the app will attempt to relaunch elevated at startup.

## Build (CMake)

From the repository root, in PowerShell:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64"
cmake --build build --config Release --target QuickReports --target QuickReportsLauncher
```

Notes:

- `CMakeLists.txt` links against Qt6 modules: `Core`, `Gui`, `Widgets`, `Network`, `Multimedia`, `MultimediaWidgets`.
- If your Qt install path differs, update `-DCMAKE_PREFIX_PATH` (or edit `CMakeLists.txt`’s `CMAKE_PREFIX_PATH`).

## Create a portable distribution (recommended)

This project includes `deploy.ps1`, which builds the targets, runs `windeployqt`, and produces a portable folder layout.

```powershell
.\deploy.ps1 -Config Release -QtPrefixPath "C:/Qt/6.10.2/msvc2022_64"
```

Output layout (portable):

- `Deployment/QuickReports.exe` (launcher stub)
- `Deployment/QuickReportsDist/` (packaged QuickReports runtime)

Run:

```powershell
.\Deployment\QuickReports.exe
```

## Runtime files expected in the portable root

Some actions expect extra files next to the launcher (i.e., in the folder containing `QuickReports.exe`):

- A `Softwares` folder containing installers (`.iso` / `.img` / `.exe`) for **Install Softwares**

## Logs

The app installs a crash/log handler that appends to `log.txt`.

In the portable layout, `log.txt` is written next to `QuickReports.exe` (one level above `QuickReportsDist`).

You can also show/hide the in-app log panel via the **Show Log** button.

## Important safety notes

- Disk actions are destructive. Be careful with disk selection; the UI disables some actions for the system drive, but other operations still erase/overwrite data.
- The app uses elevated operations for multiple utilities, so review actions before running on production machines.

## Development notes

- Qt resources are defined in `src/resources.qrc` (icons/splash/background). Ensure the referenced asset files exist for a successful build.
- UI strings are defined in `src/Translations.cpp` as a single `key → { en, fr, zh }` table; add a new key there in one place.
