// =============================================================================
// RawInputManager.h - shared Windows Raw Input source for keyboard & mouse tests.
// =============================================================================
#ifndef RAWINPUTMANAGER_H
#define RAWINPUTMANAGER_H

#include <QObject>
#include <deque>

// Singleton Raw Input source shared by the keyboard and mouse modules.
//
// Owns a private message-only window whose WndProc receives WM_INPUT, so no
// Qt window needs focus and MainWindow::nativeEvent (WM_DEVICECHANGE) stays
// untouched. The window is created on the GUI thread and Qt's Windows event
// dispatcher pumps its messages, so signals are emitted on the GUI thread.
//
// Registration is refcounted: modules call acquire*() in moduleEntered() and
// release*() in moduleLeft(), so Raw Input costs nothing while the tester is
// not in use.
//
// Mouse move events are NOT re-emitted as signals (an 8 kHz mouse would mean
// 8000 signal emissions per second); the manager aggregates timestamps and
// modules poll the rate statistics from a UI timer instead.
class RawInputManager : public QObject {
    Q_OBJECT
public:
    // The single shared instance.
    static RawInputManager &instance();

    // Refcounted: first acquire registers keyboard Raw Input, last release unregisters.
    void acquireKeyboard();
    void releaseKeyboard();
    void acquireMouse();
    void releaseMouse();

    // --- Mouse polling-rate statistics (move events only) ---
    // Events/second over the trailing `windowMs`, 0 when motion stopped.
    double currentRate(qint64 windowMs = 100) const;
    // Highest events/second seen in any sliding 1 s window.
    double peakRate() const { return m_peakRate; }
    // Median of completed active-second counts (a second counts as active
    // when it saw at least 50 move events).
    double sustainedRate() const;
    void resetMouseStats();

signals:
    // code = scanCode | (e0 ? 0x100 : 0) | (e1 ? 0x200 : 0) — identifies the
    // physical key. pressed excludes typematic auto-repeats.
    void keyEvent(quint32 code, quint16 vkey, bool pressed);
    void mouseButton(int button, bool pressed);          // 0=L 1=R 2=M 3=X1 4=X2
    void mouseWheel(bool horizontal, int delta);         // delta in wheel notches * 120
    void mouseMovedSample(int dx, int dy);               // throttled to ~60 Hz for trail UIs

private:
    RawInputManager();
    ~RawInputManager() override;
    Q_DISABLE_COPY_MOVE(RawInputManager)

    // Creates the hidden message-only window on first use.
    void ensureWindow();
    // Calls RegisterRawInputDevices to match the current refcounts.
    void updateRegistration(bool keyboard, bool mouse);
    // Decodes one WM_INPUT packet and emits the matching signal / updates statistics.
    void handleRawInput(void *hRawInput);
    // Current QueryPerformanceCounter ticks (high-resolution timestamps).
    static qint64 qpcNow();
    // QueryPerformanceCounter frequency (ticks per second).
    static qint64 qpcFreq();

    friend struct RawInputWndProcThunk;
    void *m_hwnd = nullptr; // HWND of the message-only window
    int m_kbRefs = 0;
    int m_mouseRefs = 0;
    bool m_kbRegistered = false;
    bool m_mouseRegistered = false;

    // Move-event timestamp ring (QPC ticks), pruned to the trailing 1 s.
    std::deque<qint64> m_moveTicks;
    double m_peakRate = 0.0;
    std::deque<int> m_activeSecondCounts; // completed seconds with >=50 events
    qint64 m_currentSecondStart = 0;
    int m_currentSecondCount = 0;
    qint64 m_lastMoveSampleTick = 0;      // throttle for mouseMovedSample
};

#endif // RAWINPUTMANAGER_H
