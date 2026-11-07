// =============================================================================
// PowerMetrics.cpp - implementation of PowerMetrics::read().
// =============================================================================
#include "PowerMetrics.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <comdef.h>
#include <wbemidl.h>

#include <QString>

#pragma comment(lib, "wbemuuid.lib")

namespace {

// BatteryStatus in ROOT\WMI reports voltage/rate as plain integers, but the
// exact VARIANT type varies by driver — accept every numeric shape (and a
// stringified number as a last resort).
// Converts a numeric VARIANT (any integer/real type) to double.
bool variantToDouble(const VARIANT &v, double *out) {
    switch (v.vt) {
    case VT_I4:  *out = v.lVal;   return true;
    case VT_UI4: *out = v.ulVal;  return true;
    case VT_I2:  *out = v.iVal;   return true;
    case VT_UI2: *out = v.uiVal;  return true;
    case VT_I8:  *out = double(v.llVal);  return true;
    case VT_UI8: *out = double(v.ullVal); return true;
    case VT_R8:  *out = v.dblVal; return true;
    case VT_R4:  *out = v.fltVal; return true;
    case VT_BSTR:
        if (v.bstrVal) {
            bool ok = false;
            double d = QString::fromWCharArray(v.bstrVal).trimmed().toDouble(&ok);
            if (ok) { *out = d; return true; }
        }
        return false;
    default: return false;
    }
}

// Converts a VARIANT to bool.
bool variantToBool(const VARIANT &v) {
    if (v.vt == VT_BOOL) return v.boolVal != VARIANT_FALSE;
    double d = 0;
    return variantToDouble(v, &d) && d != 0;
}

// Reads a numeric property of a WMI object; sets *known when present.
double getNum(IWbemClassObject *obj, const wchar_t *prop, bool *known = nullptr) {
    VARIANT var;
    VariantInit(&var);
    double out = 0;
    bool ok = false;
    if (SUCCEEDED(obj->Get(prop, 0, &var, nullptr, nullptr)))
        ok = variantToDouble(var, &out);
    VariantClear(&var);
    if (known) *known = ok;
    return ok ? out : 0.0;
}

// Reads a boolean property of a WMI object.
bool getBool(IWbemClassObject *obj, const wchar_t *prop) {
    VARIANT var;
    VariantInit(&var);
    bool out = false;
    if (SUCCEEDED(obj->Get(prop, 0, &var, nullptr, nullptr)))
        out = variantToBool(var);
    VariantClear(&var);
    return out;
}

// Reads live voltage / rate / flags from ROOT\WMI BatteryStatus, which reports
// real values on machines where the raw battery IOCTL returns "unknown".
// Reads live battery data from the ROOT\WMI namespace via COM (voltage,
// charge/discharge rate, PowerOnline, charging/discharging flags) and fills the
// PowerReading.
void readBatteryWmi(PowerReading &r) {
    const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownCom = (hrInit == S_OK || hrInit == S_FALSE);

    IWbemLocator *locator = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IWbemLocator,
                                   reinterpret_cast<void **>(&locator)))) {
        IWbemServices *services = nullptr;
        const HRESULT hc = locator->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr,
                                                  nullptr, nullptr, 0, nullptr,
                                                  nullptr, &services);
        locator->Release();
        if (SUCCEEDED(hc) && services) {
            CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                              nullptr, RPC_C_AUTHN_LEVEL_CALL,
                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

            IEnumWbemClassObject *en = nullptr;
            const HRESULT hq = services->ExecQuery(
                _bstr_t("WQL"),
                _bstr_t("SELECT Voltage, ChargeRate, DischargeRate, Charging, "
                        "Discharging, PowerOnline FROM BatteryStatus"),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
            if (SUCCEEDED(hq) && en) {
                IWbemClassObject *obj = nullptr;
                ULONG returned = 0;
                if (en->Next(2000, 1, &obj, &returned) == S_OK && returned) {
                    r.hasBattery = true;
                    r.charging = getBool(obj, L"Charging");
                    r.discharging = getBool(obj, L"Discharging");
                    // PowerOnline from the battery driver is the most direct AC signal.
                    r.acKnown = true;
                    r.onAc = getBool(obj, L"PowerOnline");

                    bool vKnown = false;
                    double mv = getNum(obj, L"Voltage", &vKnown);
                    if (vKnown && mv > 0) {
                        r.voltageKnown = true;
                        r.voltageV = mv / 1000.0; // mV -> V
                    }

                    // One of ChargeRate / DischargeRate is populated (mW); combine
                    // into a signed rate: positive = charging, negative = discharging.
                    double charge = getNum(obj, L"ChargeRate");
                    double discharge = getNum(obj, L"DischargeRate");
                    double mw = charge - discharge;
                    r.rateKnown = true;
                    r.powerW = mw / 1000.0; // mW -> W
                    if (r.voltageKnown && r.voltageV > 0.01)
                        r.currentA = r.powerW / r.voltageV; // I = P / V

                    obj->Release();
                }
                en->Release();
            }
            services->Release();
        }
    }
    if (ownCom)
        CoUninitialize();
}

} // namespace

// AC line + percentage via GetSystemPowerStatus, then WMI for the electrical values.
PowerReading PowerMetrics::read() {
    PowerReading r;

    // AC line + battery percentage from the simple, always-available API.
    SYSTEM_POWER_STATUS sps;
    if (GetSystemPowerStatus(&sps)) {
        if (sps.ACLineStatus != 255) { // 255 = unknown
            r.acKnown = true;
            r.onAc = (sps.ACLineStatus == 1);
        }
        if (sps.BatteryLifePercent != 255)
            r.percent = sps.BatteryLifePercent;
    }

    // Live voltage / current / wattage (overrides the AC flag with the battery
    // driver's PowerOnline when a battery is present).
    readBatteryWmi(r);
    return r;
}
