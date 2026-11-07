// =============================================================================
// RawInputManager.cpp - hidden Raw Input window, device registration and event decoding.
// =============================================================================
#include "RawInputManager.h"

#include <QDebug>
#include <algorithm>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
const wchar_t *kWndClass = L"QuickReportsRawInputWnd";

LRESULT CALLBACK rawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
} // namespace

// Gives the file-local WndProc access to the singleton's handler without
// exposing handleRawInput publicly.
// Lets the file-local window procedure call the private handleRawInput().
struct RawInputWndProcThunk {
    static void dispatch(LPARAM lParam) {
        RawInputManager::instance().handleRawInput(reinterpret_cast<void *>(lParam));
    }
};

namespace {
// Window procedure of the hidden window: forwards WM_INPUT, defers the rest.
LRESULT CALLBACK rawInputWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT) {
        RawInputWndProcThunk::dispatch(lParam);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// Meyers singleton (created on first use on the GUI thread).
RawInputManager &RawInputManager::instance() {
    static RawInputManager mgr; // constructed on first use, on the GUI thread
    return mgr;
}

RawInputManager::RawInputManager() = default;

// Destroys the hidden window.
RawInputManager::~RawInputManager() {
    if (m_hwnd)
        DestroyWindow(static_cast<HWND>(m_hwnd));
}

// (see header)
qint64 RawInputManager::qpcNow() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

// (see header)
qint64 RawInputManager::qpcFreq() {
    static const qint64 f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

// (see header) Registers the window class and creates a HWND_MESSAGE window.
void RawInputManager::ensureWindow() {
    if (m_hwnd)
        return;
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = rawInputWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWndClass;
        RegisterClassW(&wc);
        classRegistered = true;
    }
    m_hwnd = CreateWindowExW(0, kWndClass, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_hwnd)
        qWarning() << "RawInputManager: failed to create message window";
}

// (see header) Registers with RIDEV_INPUTSINK so input arrives even without focus;
// removes the registration (RIDEV_REMOVE) when no module needs the device.
void RawInputManager::updateRegistration(bool keyboard, bool mouse) {
    ensureWindow();
    if (!m_hwnd)
        return;

    std::vector<RAWINPUTDEVICE> devices;
    if (keyboard != m_kbRegistered) {
        RAWINPUTDEVICE d = {};
        d.usUsagePage = 0x01; // Generic Desktop
        d.usUsage = 0x06;     // Keyboard
        // INPUTSINK: deliver even without focus, so PrtSc/Win etc. that the
        // OS partly swallows still reach us. Never NOLEGACY — the rest of the
        // app must keep receiving normal key messages.
        d.dwFlags = keyboard ? RIDEV_INPUTSINK : RIDEV_REMOVE;
        d.hwndTarget = keyboard ? static_cast<HWND>(m_hwnd) : nullptr;
        devices.push_back(d);
    }
    if (mouse != m_mouseRegistered) {
        RAWINPUTDEVICE d = {};
        d.usUsagePage = 0x01;
        d.usUsage = 0x02; // Mouse
        d.dwFlags = mouse ? RIDEV_INPUTSINK : RIDEV_REMOVE;
        d.hwndTarget = mouse ? static_cast<HWND>(m_hwnd) : nullptr;
        devices.push_back(d);
    }
    if (devices.empty())
        return;
    if (!RegisterRawInputDevices(devices.data(), static_cast<UINT>(devices.size()),
                                 sizeof(RAWINPUTDEVICE))) {
        qWarning() << "RawInputManager: RegisterRawInputDevices failed" << GetLastError();
        return;
    }
    m_kbRegistered = keyboard;
    m_mouseRegistered = mouse;
}

// Reference counting: modules acquire when entered and release when left.
void RawInputManager::acquireKeyboard() {
    if (++m_kbRefs == 1)
        updateRegistration(true, m_mouseRegistered);
}

void RawInputManager::releaseKeyboard() {
    if (m_kbRefs > 0 && --m_kbRefs == 0)
        updateRegistration(false, m_mouseRegistered);
}

void RawInputManager::acquireMouse() {
    if (++m_mouseRefs == 1)
        updateRegistration(m_kbRegistered, true);
}

void RawInputManager::releaseMouse() {
    if (m_mouseRefs > 0 && --m_mouseRefs == 0)
        updateRegistration(m_kbRegistered, false);
}

// (see header) Keyboard: builds the scan-code based key id, ignores auto-repeat.
// Mouse: button flags, wheel deltas and move-rate bookkeeping.
void RawInputManager::handleRawInput(void *hRawInput) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    if (GetRawInputData(static_cast<HRAWINPUT>(hRawInput), RID_INPUT, &ri, &size,
                        sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;

    const qint64 now = qpcNow();

    if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD &kb = ri.data.keyboard;
        // VKey 255 marks fake/companion strokes (e.g. the E0-shift preceding
        // numpad keys, the trailing 0x45 of Pause) — never real keys.
        if (kb.VKey == 0xFF)
            return;
        const bool pressed = !(kb.Flags & RI_KEY_BREAK);
        quint32 code = kb.MakeCode & 0xFF;
        if (kb.Flags & RI_KEY_E0) code |= 0x100;
        if (kb.Flags & RI_KEY_E1) code |= 0x200;
        emit keyEvent(code, kb.VKey, pressed);
        return;
    }

    if (ri.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE &m = ri.data.mouse;
        const USHORT bf = m.usButtonFlags;

        static const struct { USHORT down, up; int idx; } kButtons[] = {
            {RI_MOUSE_LEFT_BUTTON_DOWN,   RI_MOUSE_LEFT_BUTTON_UP,   0},
            {RI_MOUSE_RIGHT_BUTTON_DOWN,  RI_MOUSE_RIGHT_BUTTON_UP,  1},
            {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2},
            {RI_MOUSE_BUTTON_4_DOWN,      RI_MOUSE_BUTTON_4_UP,      3},
            {RI_MOUSE_BUTTON_5_DOWN,      RI_MOUSE_BUTTON_5_UP,      4},
        };
        for (const auto &b : kButtons) {
            if (bf & b.down) emit mouseButton(b.idx, true);
            if (bf & b.up)   emit mouseButton(b.idx, false);
        }
        if (bf & RI_MOUSE_WHEEL)
            emit mouseWheel(false, static_cast<SHORT>(m.usButtonData));
        if (bf & RI_MOUSE_HWHEEL)
            emit mouseWheel(true, static_cast<SHORT>(m.usButtonData));

        // Movement: aggregate for polling-rate stats; only relative reports
        // count (absolute reports come from tablets/RDP and have no rate).
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX != 0 || m.lLastY != 0)) {
            const qint64 freq = qpcFreq();

            m_moveTicks.push_back(now);
            const qint64 oneSecAgo = now - freq;
            while (!m_moveTicks.empty() && m_moveTicks.front() < oneSecAgo)
                m_moveTicks.pop_front();
            m_peakRate = std::max(m_peakRate, double(m_moveTicks.size()));

            // Per-second buckets for the sustained (median) figure.
            if (m_currentSecondStart == 0)
                m_currentSecondStart = now;
            if (now - m_currentSecondStart >= freq) {
                if (m_currentSecondCount >= 50) {
                    m_activeSecondCounts.push_back(m_currentSecondCount);
                    if (m_activeSecondCounts.size() > 300)
                        m_activeSecondCounts.pop_front();
                }
                m_currentSecondStart = now;
                m_currentSecondCount = 0;
            }
            ++m_currentSecondCount;

            // Trail drawing needs ~60 Hz at most.
            if (now - m_lastMoveSampleTick > freq / 60) {
                m_lastMoveSampleTick = now;
                emit mouseMovedSample(m.lLastX, m.lLastY);
            }
        }
    }
}

// (see header)
double RawInputManager::currentRate(qint64 windowMs) const {
    if (m_moveTicks.empty())
        return 0.0;
    const qint64 freq = qpcFreq();
    const qint64 now = qpcNow();
    // Motion stopped → rate is 0, not "last window's rate".
    if (now - m_moveTicks.back() > freq / 20) // 50 ms gap
        return 0.0;
    const qint64 windowTicks = freq * windowMs / 1000;
    const qint64 cutoff = now - windowTicks;
    int n = 0;
    for (auto it = m_moveTicks.rbegin(); it != m_moveTicks.rend() && *it >= cutoff; ++it)
        ++n;
    return n * 1000.0 / double(windowMs);
}

// (see header)
double RawInputManager::sustainedRate() const {
    if (m_activeSecondCounts.empty())
        return 0.0;
    std::vector<int> v(m_activeSecondCounts.begin(), m_activeSecondCounts.end());
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return double(v[mid]);
}

// (see header)
void RawInputManager::resetMouseStats() {
    m_moveTicks.clear();
    m_peakRate = 0.0;
    m_activeSecondCounts.clear();
    m_currentSecondStart = 0;
    m_currentSecondCount = 0;
}
