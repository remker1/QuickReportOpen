# =============================================================================
# QuickReports - CMake build script (Windows / MSVC / Qt 6)
#
# Targets:
#   QuickReports          Main Qt Widgets GUI application (hardware report,
#                         disk tools, hardware tester).
#   QuickReportsLauncher  Tiny elevated bootstrap exe that starts
#                         QuickReportsDist\QuickReports.exe (see src/Launcher.cpp).
#
# Build:  cmake -S . -B build && cmake --build build --config Release
# The post-build step runs windeployqt to copy the required Qt DLLs next to
# the executable; deploy.ps1 then assembles the portable package.
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(QuickReports VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set Qt path explicitly
# Note: Please double-check this path to ensure it matches your Qt installation for MSVC
set(CMAKE_PREFIX_PATH "C:/Qt/6.10.2/msvc2022_64")

# Qt modules: Widgets = UI, Network = update/online checks, Multimedia(+Widgets)
# = camera and audio tester modules.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets)

# Automatically run MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Main application. WIN32 = GUI subsystem (no console window).
# Source groups: core window/actions/disk ops, system info + hardware model,
# dialogs, and the hardwaretester/ plug-in style test modules.
qt_add_executable(QuickReports WIN32
    src/main.cpp
    src/MainWindow.cpp src/MainWindow.h
    src/MainWindow_Actions.cpp
    src/MainWindow_DiskOps.cpp
    src/Translations.cpp src/Translations.h
    src/CrashHandler.cpp src/CrashHandler.h
    src/HardwareModel.cpp src/HardwareModel.h
    src/SystemInfoWidget.cpp src/SystemInfoWidget.h
    src/CloneProgressDialog.cpp src/CloneProgressDialog.h
    src/NewVolumeWizard.cpp src/NewVolumeWizard.h
    src/hardwaretester/HardwareTesterWidget.cpp src/hardwaretester/HardwareTesterWidget.h
    src/hardwaretester/TesterModule.h
    src/hardwaretester/TesterTranslations.cpp src/hardwaretester/TesterTranslations.h
    src/hardwaretester/common/Card.cpp src/hardwaretester/common/Card.h
    src/hardwaretester/common/TesterTheme.h
    src/hardwaretester/common/RawInputManager.cpp src/hardwaretester/common/RawInputManager.h
    src/hardwaretester/KeyboardTestModule.cpp src/hardwaretester/KeyboardTestModule.h
    src/hardwaretester/TrackpadTestModule.cpp src/hardwaretester/TrackpadTestModule.h
    src/TouchTestWidget.cpp src/TouchTestWidget.h
    src/hardwaretester/TouchTestModule.cpp src/hardwaretester/TouchTestModule.h
    src/hardwaretester/common/DisplayInfoProvider.cpp src/hardwaretester/common/DisplayInfoProvider.h
    src/hardwaretester/ScreenTestModule.cpp src/hardwaretester/ScreenTestModule.h
    src/hardwaretester/CameraTestModule.cpp src/hardwaretester/CameraTestModule.h
    src/hardwaretester/AudioTestModule.cpp src/hardwaretester/AudioTestModule.h
    src/hardwaretester/common/PowerMetrics.cpp src/hardwaretester/common/PowerMetrics.h
    src/hardwaretester/PowerTestModule.cpp src/hardwaretester/PowerTestModule.h
    src/hardwaretester/common/FingerprintScanner.cpp src/hardwaretester/common/FingerprintScanner.h
    src/hardwaretester/FingerprintTestModule.cpp src/hardwaretester/FingerprintTestModule.h
    src/resources.qrc
)

target_link_libraries(QuickReports PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    Qt6::Multimedia Qt6::MultimediaWidgets)
# Expose the CMake project version to the code as a string macro.
target_compile_definitions(QuickReports PRIVATE QUICKREPORTS_VERSION="${PROJECT_VERSION}")
if(WIN32)
    # Windows system libraries:
    #   setupapi/cfgmgr32 - device enumeration     pdh      - performance counters
    #   powrprof          - power / battery info   d3d11/dxgi/d3dcompiler - GPU & display info
    #   winbio            - Windows Biometric Framework (fingerprint tester)
    target_link_libraries(QuickReports PRIVATE setupapi cfgmgr32 pdh powrprof d3d11 d3dcompiler dxgi winbio)
endif()

# Enable link-time / whole-program optimization for Release builds (faster binary).
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if(ipo_ok)
    set_property(TARGET QuickReports PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if(WIN32)
    add_executable(QuickReportsLauncher WIN32 src/Launcher.cpp)
    set_target_properties(QuickReportsLauncher PROPERTIES OUTPUT_NAME "QuickReportsLauncher")
    if(MSVC)
        # Make launcher self-contained (no VC++ runtime DLL requirement).
        set_property(TARGET QuickReportsLauncher PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        # Request elevation. The launcher spawns the real app with CreateProcess,
        # which inherits this elevated token, so the app (and its WinBio
        # fingerprint capture from the system pool) runs with admin rights.
        target_link_options(QuickReportsLauncher PRIVATE
            "/MANIFESTUAC:level='requireAdministrator'")
    endif()
endif()

# AUTOMATIC DEPLOYMENT
# After each build of QuickReports, windeployqt copies the Qt DLLs, plugins and
# the compiler runtime beside the exe so the build folder is runnable as-is.
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${CMAKE_PREFIX_PATH}/bin")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET QuickReports POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --verbose 0
                --compiler-runtime
                --dir "$<TARGET_FILE_DIR:QuickReports>"
                "$<TARGET_FILE:QuickReports>"
            COMMENT "Running windeployqt to copy Qt DLLs..."
        )
    else()
        message(WARNING "windeployqt not found! You may need to copy DLLs manually.!")
    endif()
endif()
