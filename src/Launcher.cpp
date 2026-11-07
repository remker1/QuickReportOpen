// =============================================================================
// Launcher.cpp - tiny stand-alone bootstrap executable ("QuickReports.exe" in
// the portable package).
//
// Purpose
// -------
// The portable deployment (see deploy.ps1) keeps the real application and all
// of its Qt/runtime DLLs inside a "QuickReportsDist" sub-folder, so that the
// folder the user sees only contains one clean executable. This launcher is
// that executable. It:
//   1. Works out where it lives on disk.
//   2. Verifies that <launcher dir>\QuickReportsDist\QuickReports.exe exists.
//   3. Starts it with the dist folder as working directory.
//   4. Exits immediately (it does not wait for the app to finish).
//
// Elevation
// ---------
// CMakeLists.txt links this target with a "requireAdministrator" manifest, so
// Windows shows the UAC prompt for the launcher. Because CreateProcess children
// inherit the parent's (elevated) token, the real app starts as administrator
// too, which the disk tools and WinBio fingerprint capture need.
//
// The launcher is statically linked against the MSVC runtime so it runs on a
// machine that has no Visual C++ redistributable installed yet.
// =============================================================================
#include <windows.h>
#include <string>

// GUI-subsystem entry point (wide-char). All parameters are unused: the
// launcher takes no command line arguments.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // --- Step 1: find our own full path ------------------------------------
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0) {
        MessageBoxW(nullptr, L"Failed to locate launcher path.", L"QuickReports", MB_ICONERROR | MB_OK);
        return 1;
    }

    // Strip the file name to get the directory that contains the launcher.
    // If (unexpectedly) there is no separator, fall back to the current dir.
    std::wstring path(modulePath);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring baseDir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    const std::wstring appPath = baseDir + L"\\QuickReportsDist\\QuickReports.exe";

    // --- Step 2: make sure the real application is where we expect it ------
    DWORD attrs = GetFileAttributesW(appPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(nullptr,
                    L"QuickReportsDist\\QuickReports.exe was not found.\nPlease keep the QuickReportsDist folder next to QuickReports.exe.",
                    L"QuickReports",
                    MB_ICONERROR | MB_OK);
        return 2;
    }

    // --- Step 3: start the real application --------------------------------
    // CreateProcessW may modify the command-line buffer, hence the mutable
    // std::wstring (.data()) instead of a string literal. The path is quoted so
    // folders containing spaces work.
    std::wstring cmdLine = L"\"" + appPath + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Run with the dist folder as working directory so relative resources
    // (plugins, tools) resolve next to the real executable.
    std::wstring workDir = baseDir + L"\\QuickReportsDist";

    BOOL ok = CreateProcessW(
        appPath.c_str(),   // application to run
        cmdLine.data(),    // command line (mutable buffer)
        nullptr,           // default process security
        nullptr,           // default thread security
        FALSE,             // do not inherit handles
        0,                 // no special creation flags
        nullptr,           // inherit environment
        workDir.c_str(),   // working directory
        &si,
        &pi
    );

    if (!ok) {
        MessageBoxW(nullptr, L"Failed to launch QuickReports from QuickReportsDist.", L"QuickReports", MB_ICONERROR | MB_OK);
        return 3;
    }

    // We do not need to track the child: release both handles and quit.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
