// =============================================================================
// PowerMetrics.h - live AC / battery / voltage / current / power readings.
// =============================================================================
#ifndef POWERMETRICS_H
#define POWERMETRICS_H

// Live power / charging telemetry, read fresh on every call (cheap enough for a
// 1 Hz UI tick). This is deliberately NOT about battery health — capacity and
// wear live on the System Summary page. Here we answer two questions a
// technician cares about while probing a charging port or charger:
//   1. Is the machine on AC (charger) or on the battery right now?
//   2. What voltage / current / wattage is flowing this instant?
struct PowerReading {
    // AC line
    bool acKnown = false;   // GetSystemPowerStatus reported a definite state
    bool onAc = false;      // true = charger connected

    // Battery presence / state
    bool hasBattery = false;
    bool charging = false;
    bool discharging = false;
    bool critical = false;
    int  percent = -1;      // 0..100, -1 if unknown

    // Instantaneous electrical readings (from the battery, when present)
    bool   voltageKnown = false;
    double voltageV = 0.0;

    bool   rateKnown = false;
    double powerW = 0.0;    // signed: > 0 charging, < 0 discharging
    double currentA = 0.0;  // signed, same sign convention as powerW
};

namespace PowerMetrics {
// One snapshot of the current power state.
PowerReading read();
}

#endif // POWERMETRICS_H
