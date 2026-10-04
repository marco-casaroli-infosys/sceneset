/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2024 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "SceneSet.h"
#include "RalfPackageSupport.h"
#include <cerrno>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <limits.h>
#include <deque>
#include <poll.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <string_view>
#include <optional>

#ifndef SCENESET_TELEMETRY_METRICS_SUPPORT
#define SCENESET_TELEMETRY_METRICS_SUPPORT 0
#endif

#if SCENESET_TELEMETRY_METRICS_SUPPORT
#include <telemetry_busmessage_sender.h>
#endif

#ifndef ENABLE_FIRMWARE_CHANGE_DETECTION
#define ENABLE_FIRMWARE_CHANGE_DETECTION 0
#endif

#if ENABLE_FIRMWARE_CHANGE_DETECTION
#include "WPEFramework/interfaces/ISystemServices.h"
#endif

#ifndef GIT_SHORT_SHA
#define GIT_SHORT_SHA "unknown"
#endif

#ifndef SCENESET_DEFAULT_APPNAME
#define SCENESET_DEFAULT_APPNAME ""
#endif

#ifndef FACTORY_APP_PATH
#define FACTORY_APP_PATH ""
#endif

#ifndef APP_PREINSTALL_DIRECTORY
#define APP_PREINSTALL_DIRECTORY ""
#endif

#ifndef ENABLE_SYSTEM_CONFIG
#define ENABLE_SYSTEM_CONFIG 0
#endif

#ifndef ENABLE_CONFIG_OVERRIDE
#define ENABLE_CONFIG_OVERRIDE 0
#endif

#ifndef RESTART_HOMEAPP_ALWAYS
#define RESTART_HOMEAPP_ALWAYS 0
#endif

#ifndef DISABLE_CRASH_RECOVERY
#define DISABLE_CRASH_RECOVERY 0
#endif

#ifndef DISABLE_HOMEAPP_RESTART_ON_NEW_VERSION
#define DISABLE_HOMEAPP_RESTART_ON_NEW_VERSION 0
#endif

#define SCENESET_CONFIG_FILE "/opt/sceneset_app.conf"
#define SCENESET_SYSTEM_CONFIG_FILE "/etc/sceneset.conf"
#define SCENESET_OVERRIDE_CONFIG_FILE "/opt/sceneset.conf"
#define FIRMWARE_VERSION_FILE "/version.txt"
#define LAST_FIRMWARE_VERSION_MARKER "/opt/persistent/.sceneset_last_firmware_version"
#define FACTORY_APPS_COPIED_MARKER "/opt/persistent/.sceneset_factory_apps_copied"

namespace { // begin file-private constants and helpers
constexpr const char* kAppPackageManagerCallsign = "org.rdk.AppPackageManager";
constexpr const char* kPackageManagerDownloadDirKey = "downloadDir";
constexpr const char* kPreinstallDirectoryKey = "appPreinstallDirectory";
constexpr const char* kPreinstallLocationSettingKey = "preinstallLocation";
constexpr const char* kDefaultHomeAppSettingKey = "defaultHomeApp";
#if ENABLE_FIRMWARE_CHANGE_DETECTION
constexpr const char* kSystemServicesCallsign = "org.rdk.System";
#endif
constexpr const char* kInitialDownloadSweepEnvVar = "SCENESET_INITIAL_DOWNLOAD_SWEEP";
constexpr const char* kSceneSetSystemConfigEnvVar = "SCENESET_SYSTEM_CONFIG_FILE";
#if ENABLE_CONFIG_OVERRIDE
constexpr const char* kSceneSetOverrideConfigEnvVar = "SCENESET_OVERRIDE_CONFIG_FILE";
#endif
constexpr const char* kPackageInstallStateInstalled = "INSTALLED";
constexpr const char* kPackageInstallStateInstalling = "INSTALLING";
constexpr const char* kSceneSetHomeAppLaunchMarker = "ENTS_INFO_Sceneset_LaunchTime";
constexpr std::chrono::milliseconds kDownloadedPackageSettleDelayMs(1000);

using MetadataExtractor = bool (*)(const std::filesystem::path&, std::string&, std::string&);
MetadataExtractor g_metadataExtractor = &ralf_support::ExtractPackageMetadata;

uint64_t monotonicTimestampMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool flushFileData(const std::filesystem::path& filePath) {
    int openFlags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    openFlags |= O_NOFOLLOW;
#endif
    const int fd = ::open(filePath.c_str(), openFlags);
    if (fd < 0) {
        return false;
    }

    const int originalErrno = errno;
    if (::fsync(fd) != 0) {
        const int fsyncErrno = errno;
        ::close(fd);
        errno = fsyncErrno;
        return false;
    }
    ::close(fd);
    errno = originalErrno;
    return true;
}

bool isReadyDownloadedFile(const std::filesystem::path& filePath) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec) || ec) {
        return false;
    }

    const auto fileStatus = std::filesystem::symlink_status(filePath, ec);
    if (ec || std::filesystem::is_symlink(fileStatus)) {
        return false;
    }
    if (!std::filesystem::is_regular_file(fileStatus)) {
        return false;
    }

    const auto size = std::filesystem::file_size(filePath, ec);
    if (ec || size == 0) {
        return false;
    }
    return true;
}

bool isEnvFlagEnabled(const char* envVarName, const bool defaultValue) {
    const char* value = std::getenv(envVarName);
    if (value == nullptr) {
        return defaultValue;
    }

    std::string rawValue(value);
    std::transform(rawValue.begin(), rawValue.end(), rawValue.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

    if (rawValue == "1" || rawValue == "true" || rawValue == "yes" || rawValue == "on") {
        return true;
    }
    if (rawValue == "0" || rawValue == "false" || rawValue == "no" || rawValue == "off") {
        return false;
    }

    std::cerr << "Invalid value for " << envVarName << "='" << rawValue
              << "'. Using default=" << (defaultValue ? "enabled" : "disabled") << std::endl;
    return defaultValue;
}

std::string trimString(std::string value) {
    auto isNotSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
    return value;
}

std::string stripSurroundingQuotes(std::string value) {
    if (value.size() >= 2) {
        const char first = value.front();
        const char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return value;
}

bool splitKeyValueLine(const std::string& line, std::string& key, std::string& value) {
    const std::size_t eqPos = line.find('=');
    if (eqPos == std::string::npos) {
        return false;
    }

    key = trimString(line.substr(0, eqPos));
    value = trimString(line.substr(eqPos + 1));
    return !key.empty();
}

bool parseSystemConfig(const std::string& configPath, std::unordered_map<std::string, std::string>& values) {
    std::ifstream configFile(configPath);
    if (!configFile.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(configFile, line)) {
        std::string trimmedLine = trimString(line);
        if (trimmedLine.empty() || trimmedLine[0] == '#') {
            continue;
        }

        std::string key;
        std::string rawValue;
        if (!splitKeyValueLine(trimmedLine, key, rawValue)) {
            continue;
        }

        values[key] = trimString(stripSurroundingQuotes(rawValue));
    }

    return true;
}
} // end file-private constants and helpers

static std::string getDefaultAppName() {
    std::ifstream configFile(SCENESET_CONFIG_FILE);
    if (configFile.is_open()) {
        std::string appName;
        std::getline(configFile, appName);

        if (!appName.empty()) {
            std::cout << "Using sceneset default app from config file: " << appName << std::endl;
            return appName;
        }
    }

    std::string appDefault = SCENESET_DEFAULT_APPNAME;
    if (!appDefault.empty()) {
        std::cout << "Using sceneset default app: " << appDefault << std::endl;
    }
    return appDefault;
}

SceneSetApp::SceneSetApp()
    : m_isActive(false), m_lock(), m_appManager(nullptr), m_preinstallManager(nullptr), m_packageInstaller(nullptr), m_appManagerEventHandler(nullptr), m_preinstallManagerEventHandler(nullptr), m_packageInstallerEventHandler(nullptr), m_appmgrCallsign("org.rdk.AppManager"), m_preinstallCallsign("org.rdk.PreinstallManager"), m_referenceAppId(getDefaultAppName()), m_comrpcPath("/tmp/communicator"), m_downloadDirectory(""), m_preinstallDirectory(APP_PREINSTALL_DIRECTORY), m_launchThread(nullptr), m_stopLaunchThread(false), m_appLaunched(false), m_pendingRestart(false), m_launchThreadMutex(), m_downloadMonitorThread(nullptr), m_stopDownloadMonitorThread(false), m_downloadMonitorMutex(), m_preinstallCompletionThread(nullptr) {
    m_telemetryMetricsState.sceneSetStartTsMs = monotonicTimestampMs();
}

SceneSetApp::~SceneSetApp() {
#if !DISABLE_REFERENCE_APP_UPDATE
    stopDownloadMonitorThread();
#endif
    stopPreinstallCompletionThread();
    stopCurrentLaunchThread();
    unRegisterForAppEvents();
    unRegisterForPreinstallEvents();
    unRegisterForPackageInstallerEvents();
    releaseComInterfaces();
}

bool SceneSetApp::initialize() {
    recordSceneSetStartTimestamp();
    m_telemetryMetricsState.preinstallStartTsMs = 0;
    m_telemetryMetricsState.preinstallEndTsMs = 0;
    m_telemetryMetricsState.lastLaunchRequestTsMs = 0;
    m_telemetryMetricsState.pendingActiveTelemetry = false;
    m_telemetryMetricsState.cumulativeRelaunchCount = 0;
    m_telemetryMetricsState.lastTerminationNature = static_cast<int>(TerminationNature::NONE);
#ifdef UNIT_TEST
    m_lastTelemetryMarker.clear();
    m_lastTelemetryPayload.clear();
#endif

    // Block termination signals for this thread before initialization work;
    // new threads inherit this mask and waitForTermSignal() consumes via sigwait().
    // Save the old mask so we can restore it on early failure returns.
    sigset_t termMask, oldMask;
    sigemptyset(&termMask);
    sigaddset(&termMask, SIGTERM);
    sigaddset(&termMask, SIGINT);
    const int maskResult = pthread_sigmask(SIG_BLOCK, &termMask, &oldMask);
    if (maskResult != 0) {
        std::cerr << "Failed to block termination signals: " << strerror(maskResult) << std::endl;
        return false;
    }

    // Restore original mask on early exit (RAII-like cleanup for early returns).
    auto restoreMaskOnExit = [&oldMask]() {
        pthread_sigmask(SIG_SETMASK, &oldMask, nullptr);
    };

    const std::string envThunderAccess = getThunderAccessPath();

    std::cout << "Thunder Access Path: " << envThunderAccess << std::endl;

    Core::SystemInfo::SetEnvironment(_T("THUNDER_ACCESS"), envThunderAccess.c_str());

    auto appManagerClient = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(envThunderAccess.c_str()));

    if (!appManagerClient.IsValid()) {
        std::cerr << "Failed to create COMRPC client for AppManager." << std::endl;
        restoreMaskOnExit();
        return false;
    }

    std::cout << "AppManager COMRPC client created successfully" << std::endl;

    // Open AppManager interface
    m_appManager = appManagerClient->Open<Exchange::IAppManager>(m_appmgrCallsign.c_str());
    if (m_appManager == nullptr) {
        std::cerr << "Failed to open IAppManager interface." << std::endl;
        restoreMaskOnExit();
        return false;
    }

    std::cout << "Successfully opened " << m_appmgrCallsign << " interface" << std::endl;

    auto preinstallClient = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(envThunderAccess.c_str()));

    if (!preinstallClient.IsValid()) {
        std::cerr << "Failed to create COMRPC client for PreinstallManager." << std::endl;
        // Clean up appManager before returning
        if (m_appManager != nullptr) {
            m_appManager->Release();
            m_appManager = nullptr;
        }
        restoreMaskOnExit();
        return false;
    }

    std::cout << "PreinstallManager COMRPC client created successfully" << std::endl;
    // Open PreinstallManager interface
    m_preinstallManager = preinstallClient->Open<Exchange::IPreinstallManager>(m_preinstallCallsign.c_str());
    if (m_preinstallManager == nullptr) {
        std::cerr << "Failed to open IPreinstallManager interface." << std::endl;
        // Clean up appManager before returning
        if (m_appManager != nullptr) {
            m_appManager->Release();
            m_appManager = nullptr;
        }
        restoreMaskOnExit();
        return false;
    }

    std::cout << "Successfully opened " << m_preinstallCallsign << " interface" << std::endl;

    auto packageManagerClient = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(envThunderAccess.c_str()));

    if (!packageManagerClient.IsValid()) {
        std::cerr << "Failed to create COMRPC client for PackageManager." << std::endl;
        if (m_preinstallManager != nullptr) {
            m_preinstallManager->Release();
            m_preinstallManager = nullptr;
        }
        if (m_appManager != nullptr) {
            m_appManager->Release();
            m_appManager = nullptr;
        }
        restoreMaskOnExit();
        return false;
    }

    std::cout << "PackageManager COMRPC client created successfully" << std::endl;
    m_packageInstaller = packageManagerClient->Open<Exchange::IPackageInstaller>(kAppPackageManagerCallsign);
    if (m_packageInstaller == nullptr) {
        std::cerr << "Failed to open IPackageInstaller interface for " << kAppPackageManagerCallsign << std::endl;
        if (m_preinstallManager != nullptr) {
            m_preinstallManager->Release();
            m_preinstallManager = nullptr;
        }
        if (m_appManager != nullptr) {
            m_appManager->Release();
            m_appManager = nullptr;
        }
        restoreMaskOnExit();
        return false;
    }

    std::cout << "Successfully opened " << kAppPackageManagerCallsign << " installer interface" << std::endl;

    resolveDynamicDirectories();

    std::string preinstallDirectorySource = "PreinstallManager plugin config";
    std::unordered_map<std::string, std::string> systemConfig;
    if (loadSystemConfig(systemConfig))
    {
        const auto preinstallIt = systemConfig.find(kPreinstallLocationSettingKey);
        if (preinstallIt != systemConfig.end())
        {
            const std::string preinstallLocationFromConfig = preinstallIt->second;
            if (preinstallLocationFromConfig.empty())
            {
                std::cerr << "Ignoring empty preinstallLocation in system config" << std::endl;
            }
            else if (!std::filesystem::path(preinstallLocationFromConfig).is_absolute())
            {
                std::cerr << "Ignoring non-absolute preinstallLocation in system config: "
                          << preinstallLocationFromConfig << std::endl;
            }
            else
            {
                m_preinstallDirectory = preinstallLocationFromConfig;
                preinstallDirectorySource = "system config preinstallLocation";
            }
        }

        const auto defaultHomeAppIt = systemConfig.find(kDefaultHomeAppSettingKey);
        if (defaultHomeAppIt != systemConfig.end())
        {
            if (!defaultHomeAppIt->second.empty())
            {
                m_referenceAppId = defaultHomeAppIt->second;
                std::cout << "Using defaultHomeApp from system config: " << m_referenceAppId << std::endl;
            }
            else
            {
                std::cerr << "Ignoring empty defaultHomeApp in system config" << std::endl;
            }
        }
    }

    if (m_preinstallDirectory.empty()) {
        // Fall back to compile-time default if runtime lookup did not yield a value.
        m_preinstallDirectory = APP_PREINSTALL_DIRECTORY;
        preinstallDirectorySource = "compile-time APP_PREINSTALL_DIRECTORY";
    }

    if (m_preinstallDirectory.empty()) {
        std::cerr << "No valid appPreinstallDirectory configured, cannot proceed." << std::endl;
        if (m_preinstallManager != nullptr) {
            m_preinstallManager->Release();
            m_preinstallManager = nullptr;
        }
        if (m_packageInstaller != nullptr) {
            m_packageInstaller->Release();
            m_packageInstaller = nullptr;
        }
        if (m_appManager != nullptr) {
            m_appManager->Release();
            m_appManager = nullptr;
        }
        restoreMaskOnExit();
        return false;
    }
    std::cout << "Resolved preinstall directory from " << preinstallDirectorySource
              << ": " << m_preinstallDirectory << std::endl;
    if (m_downloadDirectory.empty()) {
        std::cout << "downloadDir is empty. Reference app update monitoring will remain disabled." << std::endl;
    }

    {
        lock_guard<mutex> lkgd(m_lock);
        m_isActive = true;
    }

#if SCENESET_TELEMETRY_METRICS_SUPPORT
    static char kT2ComponentName[] = "sceneset";
    t2_init(kT2ComponentName);
#endif

    cout << "Registered to AppManager. Waiting for term signal via sigwait" << endl;
    return m_isActive;
}

bool SceneSetApp::registerForAppEvents() {
    if (nullptr == m_appManagerEventHandler) {
        m_appManagerEventHandler = std::make_shared<AppManagerEventHandler>();
    }
    if (m_appManager != nullptr) {
        m_appManager->Register(m_appManagerEventHandler.get());
        return true;
    }
    return false;
}

bool SceneSetApp::registerForPreinstallEvents() {
    if (nullptr == m_preinstallManagerEventHandler) {
        m_preinstallManagerEventHandler = std::make_shared<PreinstallManagerEventHandler>();
    }
    if (m_preinstallManager != nullptr) {
        m_preinstallManager->Register(m_preinstallManagerEventHandler.get());
        return true;
    }
    return false;
}

bool SceneSetApp::registerForPackageInstallerEvents() {
    if (nullptr == m_packageInstallerEventHandler) {
        m_packageInstallerEventHandler = std::make_shared<PackageInstallerEventHandler>();
    }
    if (m_packageInstaller != nullptr) {
        const Core::hresult result = m_packageInstaller->Register(m_packageInstallerEventHandler.get());
        if (result != Core::ERROR_NONE) {
            std::cerr << "PackageManager Register failed: " << result << std::endl;
            return false;
        }
        return true;
    }
    return false;
}

bool SceneSetApp::unRegisterForAppEvents() {
    std::cout << " Unregistering for AppManager Events " << endl;
    if (nullptr != m_appManagerEventHandler && nullptr != m_appManager) {
        try {
            m_appManager->Unregister(m_appManagerEventHandler.get());
            std::cout << " Unregistered AppManager Events " << endl;
        } catch (...) {
            std::cerr << "Exception during AppManager unregister" << endl;
        }
    } else {
        std::cout << "AppManager or EventHandler is null, cannot unregister" << endl;
    }
    m_appManagerEventHandler = nullptr;
    return true;
}

bool SceneSetApp::unRegisterForPreinstallEvents() {
    std::cout << " Unregistering for Preinstall Events " << endl;
    if (nullptr != m_preinstallManagerEventHandler && nullptr != m_preinstallManager) {
        try {
            m_preinstallManager->Unregister(m_preinstallManagerEventHandler.get());
            std::cout << " Unregistered Preinstall Events " << endl;
        } catch (...) {
            std::cerr << "Exception during PreinstallManager unregister" << endl;
        }
    } else {
        std::cout << "PreinstallManager or EventHandler is null, cannot unregister" << endl;
    }
    m_preinstallManagerEventHandler = nullptr;
    return true;
}

bool SceneSetApp::unRegisterForPackageInstallerEvents() {
    std::cout << " Unregistering for PackageInstaller Events " << endl;
    if (nullptr != m_packageInstallerEventHandler && nullptr != m_packageInstaller) {
        try {
            m_packageInstaller->Unregister(m_packageInstallerEventHandler.get());
            std::cout << " Unregistered PackageInstaller Events " << endl;
        } catch (...) {
            std::cerr << "Exception during PackageInstaller unregister" << endl;
        }
    } else {
        std::cout << "PackageInstaller or EventHandler is null, cannot unregister" << endl;
    }
    m_packageInstallerEventHandler = nullptr;
    return true;
}

bool SceneSetApp::launchDefaultApp() {
    if (m_referenceAppId.empty()) {
        std::cout << "No reference app specified" << std::endl;
        return false;
    }
    std::cout << "Launching default app: " << m_referenceAppId << std::endl;
    Core::hresult result = m_appManager->LaunchApp(m_referenceAppId, "", "");
    if (result != Core::ERROR_NONE) {
        std::cerr << "LaunchApp failed with error code: " << result << std::endl;
        m_telemetryMetricsState.pendingActiveTelemetry = false;
        return false;
    }
    recordLaunchRequestTimestamp();
    return true;
}

bool SceneSetApp::killReferenceApp() {
    if (m_referenceAppId.empty()) {
        std::cout << "No reference app specified" << std::endl;
        return false;
    }
    if (m_appManager == nullptr) {
        std::cerr << "AppManager is not initialized" << std::endl;
        return false;
    }
    std::cout << "Killing reference app: " << m_referenceAppId << std::endl;
    Core::hresult result = m_appManager->KillApp(m_referenceAppId);
    if (result == Core::ERROR_NONE) {
        std::cout << "Successfully requested kill of reference app" << std::endl;
    }
    return true;
}

bool SceneSetApp::startPreinstall(bool forceInstall) {
    if (m_preinstallManager == nullptr) {
        std::cerr << "PreinstallManager is not initialized, cannot start preinstall." << std::endl;
        return false;
    }
    recordPreinstallStartTimestamp();
    std::cout << "Starting preinstall with forceInstall=" << (forceInstall ? "true" : "false") << std::endl;
    Core::hresult result = m_preinstallManager->StartPreinstall(forceInstall);
    if (result != Core::ERROR_NONE) {
        std::cerr << "StartPreinstall failed with error code: " << result << std::endl;
        return false;
    }
    return true;
}

bool SceneSetApp::isReferenceAppInstalled() {
    if (m_referenceAppId.empty()) {
        std::cout << "No reference app ID specified" << std::endl;
        return false;
    }

    if (m_appManager == nullptr) {
        std::cerr << "AppManager is not initialized" << std::endl;
        return false;
    }

    bool isInstalled = false;
    Core::hresult result = m_appManager->IsInstalled(m_referenceAppId, isInstalled);
    if (result != Core::ERROR_NONE) {
        std::cerr << "IsInstalled failed for reference app '" << m_referenceAppId
                  << "' with error code: " << result << std::endl;
        return false;
    }

    if (isInstalled) {
        std::cout << "Reference app '" << m_referenceAppId << "' is already installed" << std::endl;
        return true;
    }

    std::cout << "Reference app '" << m_referenceAppId << "' is not installed yet" << std::endl;
    return false;
}

bool SceneSetApp::isFactoryAppsCopied() {
#ifdef UNIT_TEST
    const std::string markerPath = m_factoryAppsCopiedMarkerOverride.empty()
        ? std::string(FACTORY_APPS_COPIED_MARKER) : m_factoryAppsCopiedMarkerOverride;
#else
    const std::string markerPath = FACTORY_APPS_COPIED_MARKER;
#endif
    if (std::filesystem::exists(markerPath)) {
        std::cout << "Factory apps marker file exists at: " << markerPath << std::endl;
        return true;
    }
    std::cout << "Factory apps marker file does not exist. This is the first boot." << std::endl;
    return false;
}

void SceneSetApp::markFactoryAppsCopied() {
#ifdef UNIT_TEST
    const std::string markerPath = m_factoryAppsCopiedMarkerOverride.empty()
        ? std::string(FACTORY_APPS_COPIED_MARKER) : m_factoryAppsCopiedMarkerOverride;
#else
    const std::string markerPath = FACTORY_APPS_COPIED_MARKER;
#endif
    std::ofstream markerFile(markerPath);
    if (markerFile.is_open()) {
        markerFile << "Factory apps copied on first boot" << std::endl;
        markerFile.close();
        if (!flushFileData(markerPath)) {
            std::cerr << "Failed to flush factory apps marker file at: " << markerPath << std::endl;
        }
        std::cout << "Factory apps marker file created at: " << markerPath << std::endl;
    } else {
        std::cerr << "Failed to create factory apps marker file at: " << markerPath << std::endl;
    }
}

#if ENABLE_FIRMWARE_CHANGE_DETECTION
std::string SceneSetApp::readCurrentFirmwareVersionFromSystem() const {
    const std::string thunderAccessPath = getThunderAccessPath();
    auto client = Core::ProxyType<RPC::CommunicatorClient>::Create(
        Core::NodeId(thunderAccessPath.c_str()));
    if (!client.IsValid()) {
        std::cerr << "Failed to create COMRPC client for SystemServices" << std::endl;
        return "";
    }

    Exchange::ISystemServices* systemServices = client->Open<Exchange::ISystemServices>(kSystemServicesCallsign);
    if (systemServices == nullptr) {
        std::cerr << "Failed to open ISystemServices interface on " << kSystemServicesCallsign << std::endl;
        return "";
    }

    Exchange::ISystemServices::DownloadedFirmwareInfo info{};
    const Core::hresult result = systemServices->GetDownloadedFirmwareInfo(info);
    systemServices->Release();

    if (result != Core::ERROR_NONE) {
        std::cerr << "GetDownloadedFirmwareInfo failed with error code: " << result << std::endl;
        return "";
    }

    if (!info.success) {
        std::cerr << "GetDownloadedFirmwareInfo reported failure: " << info.errorMessage << std::endl;
        return "";
    }

    return info.currentFWVersion;
}

std::string SceneSetApp::readCurrentFirmwareVersion() const {
    // Primary: query the System plugin via COMRPC.
    const std::string systemVersion = readCurrentFirmwareVersionFromSystem();
    if (!systemVersion.empty()) {
        std::cout << "Current firmware version from System plugin: " << systemVersion << std::endl;
        return systemVersion;
    }

    // Fallback: read directly from the firmware version file on disk.
#ifdef UNIT_TEST
    const std::string firmwareVersionFile = m_firmwareVersionFileOverride.empty()
        ? std::string(FIRMWARE_VERSION_FILE) : m_firmwareVersionFileOverride;
#else
    const std::string firmwareVersionFile = FIRMWARE_VERSION_FILE;
#endif
    std::cerr << "System plugin unavailable; falling back to firmware version file: " << firmwareVersionFile << std::endl;
    std::ifstream file(firmwareVersionFile);
    if (!file.is_open()) {
        std::cerr << "Failed to open firmware version file: " << firmwareVersionFile << std::endl;
        return "";
    }
    std::string line;
    while (std::getline(file, line)) {
        constexpr const char* prefix = "imagename:";
        constexpr std::size_t prefixLen = 10; // strlen("imagename:")
        if (line.rfind(prefix, 0) == 0) {
            std::string version = line.substr(prefixLen);
            const auto start = version.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) return "";
            const auto end = version.find_last_not_of(" \t\r\n");
            return version.substr(start, end - start + 1);
        }
    }
    std::cerr << "No 'imagename:' line found in firmware version file: " << firmwareVersionFile << std::endl;
    return "";
}

std::string SceneSetApp::readLastBootFirmwareVersion() const {
#ifdef UNIT_TEST
    const std::string markerPath = m_lastFirmwareVersionMarkerOverride.empty()
        ? std::string(LAST_FIRMWARE_VERSION_MARKER) : m_lastFirmwareVersionMarkerOverride;
#else
    const std::string markerPath = LAST_FIRMWARE_VERSION_MARKER;
#endif
    std::ifstream file(markerPath);
    if (!file.is_open()) {
        std::cout << "No last firmware version marker found at: " << markerPath << std::endl;
        return "";
    }
    std::string version;
    std::getline(file, version);
    const auto start = version.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const auto end = version.find_last_not_of(" \t\r\n");
    return version.substr(start, end - start + 1);
}

void SceneSetApp::storeCurrentFirmwareVersion() {
    const std::string currentVersion = readCurrentFirmwareVersion();
    if (currentVersion.empty()) {
        // Don't persist an unknown version; next boot retries the first-boot flow.
        std::cerr << "Current firmware version unavailable; not updating last-firmware-version marker." << std::endl;
        return;
    }
#ifdef UNIT_TEST
    const std::string markerPath = m_lastFirmwareVersionMarkerOverride.empty()
        ? std::string(LAST_FIRMWARE_VERSION_MARKER) : m_lastFirmwareVersionMarkerOverride;
#else
    const std::string markerPath = LAST_FIRMWARE_VERSION_MARKER;
#endif
    std::ofstream file(markerPath);
    if (file.is_open()) {
        file << currentVersion << std::endl;
        file.close();
        std::cout << "Stored current firmware version '" << currentVersion << "' at: " << markerPath << std::endl;
    } else {
        std::cerr << "Failed to store current firmware version at: " << markerPath << std::endl;
    }
}

bool SceneSetApp::isFirmwareChanged() const {
    const std::string currentVersion = readCurrentFirmwareVersion();
    const std::string lastVersion = readLastBootFirmwareVersion();

    if (currentVersion.empty()) {
        std::cout << "Could not determine current firmware version. Treating as firmware changed." << std::endl;
        return true;
    }

    if (lastVersion.empty()) {
        std::cout << "No previous firmware version stored. Treating as firmware changed (first boot)." << std::endl;
        return true;
    }

    if (currentVersion != lastVersion) {
        std::cout << "Firmware changed from '" << lastVersion << "' to '" << currentVersion << "'. Treating as first boot." << std::endl;
        return true;
    }

    std::cout << "Firmware version unchanged: '" << currentVersion << "'. Normal boot." << std::endl;
    return false;
}
#endif // ENABLE_FIRMWARE_CHANGE_DETECTION

bool SceneSetApp::copyFactoryAppsToPreinstall() {
    namespace fs = std::filesystem;

#ifdef UNIT_TEST
    const std::string factoryAppPath = m_factoryAppPathOverride.empty()
        ? std::string(FACTORY_APP_PATH) : m_factoryAppPathOverride;
#else
    const std::string factoryAppPath = FACTORY_APP_PATH;
#endif

    std::cout << "Copying factory apps from " << factoryAppPath << " to " << m_preinstallDirectory << std::endl;

    fs::path sourcePath(factoryAppPath);
    fs::path destPath(m_preinstallDirectory);

    if (!fs::exists(sourcePath)) {
        std::cerr << "Failed to open factory apps location: " << factoryAppPath << std::endl;
        return false;
    }

    // Create preinstall directory if it doesn't exist
    if (!fs::exists(destPath)) {
        std::cout << "Creating preinstall directory: " << m_preinstallDirectory << std::endl;
        try {
            fs::create_directories(destPath);
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Failed to create preinstall directory: " << e.what() << std::endl;
            return false;
        }
    }

    // Copy bundle files from factory location to preinstall folder
    int fileCount = 0;
    int failedCount = 0;

    try {
        for (const auto& entry : fs::directory_iterator(sourcePath)) {
            if (!fs::is_regular_file(entry.status())) {
                continue; // Skip directories and non-regular files
            }

            const std::string fileName = entry.path().filename().string();

            // Copy the bundle file directly to the preinstall directory
            fs::path destination = destPath / fileName;

            try {
                std::cout << "Copying bundle: " << fileName << " to preinstall directory" << std::endl;

                fs::copy_file(entry.path(), destination,
                             fs::copy_options::overwrite_existing);
                fileCount++;
                std::cout << "Successfully copied bundle: " << fileName << std::endl;

            } catch (const fs::filesystem_error& e) {
                std::cerr << "Failed to copy bundle: " << fileName
                          << " - " << e.what() << std::endl;
                failedCount++;
            }
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error iterating directory: " << e.what() << std::endl;
        return false;
    }

    if (fileCount > 0) {
        std::cout << "Successfully copied " << fileCount << " factory app bundles to preinstall folder" << std::endl;
    } else {
        std::cout << "No factory app bundles found to copy" << std::endl;
    }
    // The first boot is only recorded once the preinstall has succeeded (completeStartupAfterPreinstall):
    // marking it here would make an interrupted first boot look complete.
    if (failedCount > 0) {
        std::cerr << "Failed to copy " << failedCount << " factory app bundles" << std::endl;
        return false;
    }
    return true;
}

// First boot: copy the factory apps to the preinstall folder. With the marker strategy the first boot is
// recorded only after the copy and the forced preinstall succeeded (completeStartupAfterPreinstall).
void SceneSetApp::prepareFactoryAppsForFirstBoot() {
    std::cout << "First boot/Factory reset detected. Copying factory apps to preinstall folder." << std::endl;
    const bool factoryAppsCopied = copyFactoryAppsToPreinstall();
    if (!factoryAppsCopied) {
        std::cerr << "Failed to copy factory apps. Continuing with preinstall anyway." << std::endl;
    }
#if !ENABLE_FIRMWARE_CHANGE_DETECTION
    m_startupPreinstallState.markFactoryAppsCopiedOnSuccess = factoryAppsCopied;
#endif
}

void SceneSetApp::cleanupPreinstallFolder() {
    namespace fs = std::filesystem;

    std::cout << "Cleaning up preinstall folder: " << m_preinstallDirectory << std::endl;

    const fs::path preinstallPath(m_preinstallDirectory);

    if (!fs::exists(preinstallPath)) {
        std::cout << "Preinstall directory does not exist, nothing to clean up" << std::endl;
        return;
    }

    try {
        int removedCount = 0;
        for (const auto& entry : fs::directory_iterator(preinstallPath)) {
            const auto entryName = entry.path().filename().string();

            try {
                if (const auto status = entry.status(); fs::is_directory(status)) {
                    std::cout << "Removing directory: " << entryName << std::endl;
                    fs::remove_all(entry.path());
                    removedCount++;
                    std::cout << "Successfully removed directory: " << entryName << std::endl;
                } else if (fs::is_regular_file(status)) {
                    std::cout << "Removing file: " << entryName << std::endl;
                    fs::remove(entry.path());
                    removedCount++;
                    std::cout << "Successfully removed file: " << entryName << std::endl;
                }
            } catch (const fs::filesystem_error& e) {
                std::cerr << "Failed to remove: " << entryName
                          << " - " << e.what() << std::endl;
            }
        }

        if (removedCount > 0) {
            std::cout << "Successfully cleaned up " << removedCount << " items from preinstall folder" << std::endl;
        } else {
            std::cout << "No items found to clean up in preinstall folder" << std::endl;
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error cleaning up preinstall folder: " << e.what() << std::endl;
    }
}

void SceneSetApp::completeStartupAfterPreinstall() {
    bool expected = true;
    if (!m_startupPreinstallState.waitingForCompletion.compare_exchange_strong(expected, false)) {
        return;
    }

    if (!m_isActive.load()) {
        std::cout << "Preinstall phase finished after shutdown started. Skipping remaining startup work." << std::endl;
        return;
    }

    recordPreinstallEndTimestamp();

    std::cout << "Preinstall phase finished. Continuing startup flow." << std::endl;
    if (isStartupPreinstallSucceed()) {
        cleanupPreinstallFolder();
        // The installed packages must be on disk before the first boot is recorded as done:
        // after a power loss the next boot would otherwise skip packages left empty or partial.
        ::sync();
#if ENABLE_FIRMWARE_CHANGE_DETECTION
        // Record the firmware as the new datasource only after preinstall succeeds.
        storeCurrentFirmwareVersion();
#else
        if (m_startupPreinstallState.markFactoryAppsCopiedOnSuccess.exchange(false)) {
            markFactoryAppsCopied();
        }
#endif
    } else {
        std::cerr << "Startup preinstall reported a failure state before completion. Preserving files in preinstall folder for retry." << std::endl;
    }
    checkAndLaunchIfAlreadyInstalled();

#if !DISABLE_REFERENCE_APP_UPDATE
    startDownloadMonitorThread();
#endif
}

void SceneSetApp::resetStartupPreinstallStatusTracking() {
    m_startupPreinstallState.hasFailure = false;
}

void SceneSetApp::beginStartupPreinstallStatusTracking(bool statusEventsRegistered) {
    resetStartupPreinstallStatusTracking();
    if (!statusEventsRegistered) {
        // Without package status events a failed install cannot be detected: treat the preinstall as unverified.
        m_startupPreinstallState.hasFailure = true;
    }
}

void SceneSetApp::recordStartupPreinstallStatus(const std::string& jsonresponse) {
    if (jsonresponse.empty()) {
        return;
    }

    JsonArray packages;
    if (!packages.FromString(jsonresponse)) {
        std::cerr << "Failed to parse preinstall status JSON response" << std::endl;
        return;
    }

    JsonArray::Iterator index = packages.Elements();
    while (index.Next()) {
        const JsonValue& element = index.Current();
        if (element.Content() != JsonValue::type::OBJECT) {
            continue;
        }

        const JsonObject packageObj = element.Object();
        std::string packageId;
        std::string state;
        std::string version;

        if (packageObj.HasLabel("packageId")) {
            const JsonValue& packageIdValue = packageObj["packageId"];
            if (packageIdValue.Content() == JsonValue::type::STRING) {
                packageId = packageIdValue.String();
            }
        }

        if (packageObj.HasLabel("state")) {
            const JsonValue& stateValue = packageObj["state"];
            if (stateValue.Content() == JsonValue::type::STRING) {
                state = stateValue.String();
            }
        }

        if (packageObj.HasLabel("version")) {
            const JsonValue& versionValue = packageObj["version"];
            if (versionValue.Content() == JsonValue::type::STRING) {
                version = versionValue.String();
            }
        }

        std::cout << "Package: " << packageId << ", Version: " << version << ", State: " << state << std::endl;

        if (packageId.empty() || state.empty()) {
            continue;
        }

        std::string packageKey = packageId;
        if (!version.empty()) {
            packageKey += ":";
            packageKey += version;
        }

        if (state != kPackageInstallStateInstalled && state != kPackageInstallStateInstalling) {
            std::cerr << "Preinstall package '" << packageKey
                      << "' reported unexpected state '" << state << "'; marking preinstall as failed." << std::endl;
            m_startupPreinstallState.hasFailure = true;
        }
    }
}

bool SceneSetApp::isStartupPreinstallSucceed() const {
    if (m_startupPreinstallState.hasFailure) {
        std::cerr << "One or more preinstall packages reported a failure state." << std::endl;
        return false;
    }
    return true;
}

void SceneSetApp::checkAndLaunchIfAlreadyInstalled() {
    if (!m_appLaunched) {
        std::cout << "Checking if reference app is installed" << std::endl;
        if (isReferenceAppInstalled()) {
            bool expected = false;
            if (m_appLaunched.compare_exchange_strong(expected, true)) {
                std::cout << "Reference app '" << m_referenceAppId << "' is already installed. Launching default app." << std::endl;
                startLaunchThread();
            }
        } else {
            std::cout << "Reference app '" << m_referenceAppId << "' is not installed." << std::endl;
        }
    }
}

void SceneSetApp::waitForTermSignal() {
    sigset_t termMask;
    sigemptyset(&termMask);
    sigaddset(&termMask, SIGTERM);
    sigaddset(&termMask, SIGINT);

    while (m_isActive.load()) {
        int signalNumber = 0;
        const int waitResult = sigwait(&termMask, &signalNumber);
        if (waitResult != 0) {
            std::cerr << "sigwait failed: " << strerror(waitResult) << std::endl;
            continue;
        }
        onTerminate();
    }

    std::cout << "Exiting application..." << std::endl;
}

void SceneSetApp::handleTerminationSignal(int signal) {
    (void)signal;
    SceneSetApp::getInstance().onTerminate();
}

void SceneSetApp::releaseComInterfaces() {
    if (m_packageInstaller != nullptr) {
        m_packageInstaller->Release();
        m_packageInstaller = nullptr;
    }
    if (m_preinstallManager != nullptr) {
        m_preinstallManager->Release();
        m_preinstallManager = nullptr;
    }
    if (m_appManager != nullptr) {
        m_appManager->Release();
        m_appManager = nullptr;
    }
}

void SceneSetApp::onTerminate() {
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_isActive = false;
    }
    stopPreinstallCompletionThread();
#if !DISABLE_REFERENCE_APP_UPDATE
    stopDownloadMonitorThread();
#endif
    if (m_appLaunched.load()) {
        std::cout << "Stopping reference app on service shutdown." << std::endl;
        killReferenceApp();
    }
    unRegisterForAppEvents();
    unRegisterForPreinstallEvents();
    unRegisterForPackageInstallerEvents();
    releaseComInterfaces();
}

SceneSetApp& SceneSetApp::getInstance() {
    static SceneSetApp instance;
    return instance;
}

void SceneSetApp::run() {
    if (!initialize()) return;
    sd_notifyf(0, "READY=1\n"
                  "STATUS=ComRPC client is Successfully Initialized\n"
                  "MAINPID=%lu",
               (unsigned long)getpid());
    std::cout << "SceneSetApp version: " << GIT_SHORT_SHA << std::endl;
    if (m_referenceAppId.empty()) {
        std::cout << "No reference app ID specified, skipping preinstall and app launch" << std::endl;
        return;
    }
    const bool preinstallEventsRegistered = registerForPreinstallEvents();
    if (!preinstallEventsRegistered) {
        std::cerr << "Failed to register for PreinstallManager completion events" << std::endl;
    }

    const bool packageInstallerEventsRegistered = registerForPackageInstallerEvents();
    if (!packageInstallerEventsRegistered) {
        std::cerr << "Failed to register for PackageManager installation status events" << std::endl;
    }
    
    const bool appManagerEventsRegistered = registerForAppEvents();
    if (!appManagerEventsRegistered) {
        std::cerr << "Failed to register for AppManager events" << std::endl;
    }

    // Determine if this is a Factory Setting Reset (FSR) / first boot scenario.
#if ENABLE_FIRMWARE_CHANGE_DETECTION
    // A first boot is triggered whenever the current firmware version differs from
    // the firmware version recorded during the previous boot.
    bool isFactoryReset = isFirmwareChanged();
#else
    // A first boot is detected via the persistent factory-apps-copied marker file.
    bool isFactoryReset = !isFactoryAppsCopied();
#endif

    // Copy factory apps to preinstall folder on first boot only
    if (isFactoryReset) {
        prepareFactoryAppsForFirstBoot();
    } else {
        std::cout << "Not a first boot. Skipping factory app copy." << std::endl;
    }

    // Start preinstall asynchronously.
    // Use forceInstall=true for FSR cases (force reinstall all packages)
    // Use forceInstall=false for normal boots (only install if newer version)
    beginStartupPreinstallStatusTracking(packageInstallerEventsRegistered);
    m_startupPreinstallState.waitingForCompletion = true;
    std::cout << "Starting preinstall process and waiting for OnPreinstallationComplete" << std::endl;
    if (!startPreinstall(isFactoryReset)) {
        std::cerr << "Failed to start preinstall process. Continuing startup flow without deleting preinstall files." << std::endl;
        // Preinstall never ran: mark it failed so the folder is preserved and the firmware datasource is not advanced.
        m_startupPreinstallState.hasFailure = true;
        completeStartupAfterPreinstall();
    } else {
        if (!preinstallEventsRegistered) {
            std::cerr << "Preinstall started asynchronously without a registered completion handler; startup post-actions depend on PreinstallManager notification delivery." << std::endl;
        }
        if (!packageInstallerEventsRegistered) {
            std::cerr << "Preinstall started without a registered PackageManager status handler; preinstall files will be preserved because install success cannot be confirmed." << std::endl;
        }
        if (!appManagerEventsRegistered) {
            std::cerr << "Preinstall started without a registered AppManager status handler" << std::endl;
        }
    }

    waitForTermSignal();
}

// AppManagerEventHandler implementations
SceneSetApp::AppManagerEventHandler::~AppManagerEventHandler() {}

void SceneSetApp::AppManagerEventHandler::OnAppInstalled(const string &appId, const string &version) {
    std::cout << "App Installed: " << appId << " Version: " << version << std::endl;

    SceneSetApp& instance = SceneSetApp::getInstance();
    if (!instance.m_referenceAppId.empty() && appId == instance.m_referenceAppId) {
#if DISABLE_HOMEAPP_RESTART_ON_NEW_VERSION
        std::cout << "New version of reference app '" << appId << "' (version: " << version
                  << ") installed. Auto kill/relaunch feature on install is disabled." << std::endl;
#else
        if (instance.m_appLaunched) {
            std::cout << "New version of reference app '" << appId << "' (version: " << version << ") installed. App is running, killing and restarting reference app." << std::endl;
            // Kill the running app  before launching the new version
            // The lifecycle events will handle the state transitions
            instance.m_pendingRestart = true;
            if (instance.killReferenceApp()) {
                std::cout << "Kill requested. App will be restarted when it reaches UNLOADED state." << std::endl;
                // Note: The launch will be triggered by OnAppLifecycleStateChanged when app reaches UNLOADED
            } else {
                std::cerr << "Failed to kill reference app" << std::endl;
                instance.m_pendingRestart = false;
            }
        }
#endif
    }
}

void SceneSetApp::AppManagerEventHandler::OnAppUninstalled(const string &appId) {
    std::cout << "App Uninstalled: " << appId << std::endl;
}

void SceneSetApp::AppManagerEventHandler::OnAppLifecycleStateChanged(const string &appId, const string &appInstanceId, const Exchange::IAppManager::AppLifecycleState newState, const Exchange::IAppManager::AppLifecycleState oldState, const Exchange::IAppManager::AppErrorReason errorReason) {

    SceneSetApp& instance = SceneSetApp::getInstance();
    std::cout << "App Lifecycle State Changed for " << appId
              << " from " << getAppStateString(oldState) << " (" << static_cast<int>(oldState) << ")"
              << " to " << getAppStateString(newState) << " (" << static_cast<int>(newState) << ")" << std::endl;
    if (!instance.m_referenceAppId.empty() && appId == instance.m_referenceAppId) {
        // Track if reference app is running using m_appLaunched
        if (newState == Exchange::IAppManager::AppLifecycleState::APP_STATE_RUNNING ||
            newState == Exchange::IAppManager::AppLifecycleState::APP_STATE_ACTIVE) {
            instance.m_appLaunched = true;
            if (newState == Exchange::IAppManager::AppLifecycleState::APP_STATE_ACTIVE) {
                const bool shouldPublishTelemetry = instance.m_telemetryMetricsState.pendingActiveTelemetry.exchange(false);
                if (shouldPublishTelemetry) {
                    instance.publishHomeAppActiveTelemetry(monotonicTimestampMs());
                }
            }
        } else if (newState == Exchange::IAppManager::AppLifecycleState::APP_STATE_UNLOADED) {
            instance.m_appLaunched = false;

            bool shouldRestart = false;
            if (oldState == Exchange::IAppManager::AppLifecycleState::APP_STATE_TERMINATING) {
                if (instance.m_pendingRestart) {
                    instance.setLastTerminationNature(TerminationNature::INTENTIONAL_KILL);
                } else if (errorReason == Exchange::IAppManager::AppErrorReason::APP_ERROR_ABORT) {
                    instance.setLastTerminationNature(TerminationNature::CRASH);
                } else {
                    instance.setLastTerminationNature(TerminationNature::INTENTIONAL_KILL);
                }
            }

            // Check if we need to restart after new version installation
            if (instance.m_pendingRestart) {
                std::cout << "App reached UNLOADED state after new version installation. Restarting with new version." << std::endl;
                instance.m_pendingRestart = false;
                shouldRestart = true;
            }
            else if (oldState == Exchange::IAppManager::AppLifecycleState::APP_STATE_TERMINATING) {
#if RESTART_HOMEAPP_ALWAYS
                // Optional build behavior: restart on any TERMINATING->UNLOADED transition.
                std::cout << "App " << appId << " terminated. Restarting reference app due to RESTART_HOMEAPP_ALWAYS." << std::endl;
                std::cout << "Termination error reason: " << static_cast<int>(errorReason) << std::endl;
                shouldRestart = true;
#elif !DISABLE_CRASH_RECOVERY
                // Default behavior: restart only for ABORT terminations.
                // Disable with -DDISABLE_CRASH_RECOVERY=ON when HomeAppManager owns crash recovery.
                if (errorReason == Exchange::IAppManager::AppErrorReason::APP_ERROR_ABORT) {
                    std::cout << "App " << appId << " terminated with ABORT error. Restarting reference app." << std::endl;
                    shouldRestart = true;
                }
#else
                std::cout << "App " << appId << " terminated. Crash recovery disabled (DISABLE_CRASH_RECOVERY=ON)." << std::endl;
#endif
            }

            if (shouldRestart) {
                instance.m_telemetryMetricsState.cumulativeRelaunchCount.fetch_add(1);
                instance.startLaunchThread();
            }
        } else if (newState == Exchange::IAppManager::AppLifecycleState::APP_STATE_TERMINATING) {
            instance.m_appLaunched = false;
        }
    }
}

void SceneSetApp::AppManagerEventHandler::OnAppLaunchRequest(const string &appId, const string &intent, const string &source) {
    std::cout << "App Launch Request: " << appId << " Intent: " << intent << " Source: " << source << std::endl;
}

const char* SceneSetApp::AppManagerEventHandler::getAppStateString(const Exchange::IAppManager::AppLifecycleState state) {
    switch(state) {
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_UNLOADED: return "UNLOADED";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_LOADING: return "LOADING";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_INITIALIZING: return "INITIALIZING";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_PAUSED: return "PAUSED";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_RUNNING: return "RUNNING";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_ACTIVE: return "ACTIVE";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_SUSPENDED: return "SUSPENDED";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_HIBERNATED: return "HIBERNATED";
        case Exchange::IAppManager::AppLifecycleState::APP_STATE_TERMINATING: return "TERMINATING";
        default: return "UNKNOWN";
    }
}

void SceneSetApp::AppManagerEventHandler::OnAppUnloaded(const string &appId, const string &appInstanceId) {
    std::cout << "App Unloaded: " << appId << " Instance ID: " << appInstanceId << std::endl;
}

uint32_t SceneSetApp::AppManagerEventHandler::AddRef() const {
    cout << " AddRef called  " << endl;
    return Core::ERROR_NONE;
}

uint32_t SceneSetApp::AppManagerEventHandler::Release() const {
    cout << " Release called " << endl;
    return Core::ERROR_NONE;
}

/**
 * Stops the currently running launch thread, if any.
 */
void SceneSetApp::stopCurrentLaunchThread() {
    std::lock_guard<std::mutex> lock(m_launchThreadMutex);
    if (m_launchThread && m_launchThread->joinable()) {
        std::cout << "Stopping current launch thread" << std::endl;
        m_stopLaunchThread = true;
        m_launchThread->join();
        m_launchThread.reset();
        std::cout << "Launch thread stopped successfully" << std::endl;
    }
    m_stopLaunchThread = false;
}

// Launching the default app from a thread as this is also called from appmanager OnAppLifecycleStateChanged event handler
void SceneSetApp::startLaunchThread() {
    stopCurrentLaunchThread();

    m_launchThread = std::make_unique<std::thread>([this]() {
        try {
            if (!m_stopLaunchThread) {
                std::cout << "Executing launchDefaultApp from thread" << std::endl;
                if (!launchDefaultApp()) {
                    std::cerr << "Failed to launch default app from thread" << std::endl;
                }
            } else {
                std::cout << "Launch thread cancelled before execution" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Exception in launch thread: " << e.what() << std::endl;
        }
    });
}

void SceneSetApp::startDownloadMonitorThread() {
    if (m_downloadDirectory.empty()) {
        std::cout << "downloadDir is not available. Download monitor is disabled." << std::endl;
        return;
    }

    stopDownloadMonitorThread();
    {
        std::lock_guard<std::mutex> lock(m_downloadMonitorMutex);
        m_stopDownloadMonitorThread = false;
        m_downloadMonitorThread = std::make_unique<std::thread>([this]() {
            monitorDownloadDirectory();
        });
    }
}

void SceneSetApp::startPreinstallCompletionThread() {
    std::cout << "Starting Preinstall Completion Thread" << std::endl;

    if (!m_isActive.load()) {
        std::cout << "Skipping preinstall completion worker start because shutdown is already in progress." << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(m_startupPreinstallState.completionThreadMutex);
    if (m_preinstallCompletionThread && m_preinstallCompletionThread->joinable()) {
        std::cout << "Preinstall completion thread already running, skipping duplicate start." << std::endl;
        return;
    }
    m_preinstallCompletionThread = std::make_unique<std::thread>([this]() {
        completeStartupAfterPreinstall();
    });
}

void SceneSetApp::stopPreinstallCompletionThread() {
    std::unique_ptr<std::thread> threadToJoin;
    {
        std::lock_guard<std::mutex> lock(m_startupPreinstallState.completionThreadMutex);
        if (m_preinstallCompletionThread && m_preinstallCompletionThread->joinable()) {
            threadToJoin = std::move(m_preinstallCompletionThread);
        } else {
            m_preinstallCompletionThread.reset();
        }
    }
    if (threadToJoin && threadToJoin->joinable()) {
        threadToJoin->join();
    }
}

void SceneSetApp::stopDownloadMonitorThread() {
    std::unique_ptr<std::thread> threadToJoin;
    {
        std::lock_guard<std::mutex> lock(m_downloadMonitorMutex);
        m_stopDownloadMonitorThread = true;
        if (m_downloadMonitorThread && m_downloadMonitorThread->joinable()) {
            threadToJoin = std::move(m_downloadMonitorThread);
        } else {
            m_downloadMonitorThread.reset();
        }
    }
    if (threadToJoin && threadToJoin->joinable()) {
        threadToJoin->join();
    }
}

void SceneSetApp::monitorDownloadDirectory() {
    namespace fs = std::filesystem;

    int inotifyFd = -1;
    int watchFd = -1;
    std::thread settleWorker;
    std::mutex settleQueueMutex;
    std::condition_variable settleQueueCv;
    std::deque<std::pair<fs::path, std::chrono::steady_clock::time_point>> settleQueue;
    bool stopSettleWorker = false;

    try {
        const fs::path downloadDir(m_downloadDirectory);
        std::error_code ec;
        const bool downloadDirExists = fs::exists(downloadDir, ec);
        if (ec) {
            std::cerr << "Download monitor disabled. Failed to validate downloadDir '" << m_downloadDirectory
                      << "': " << ec.message() << std::endl;
            return;
        }

        const bool downloadDirIsDirectory = fs::is_directory(downloadDir, ec);
        if (ec) {
            std::cerr << "Download monitor disabled. Failed to query downloadDir type '" << m_downloadDirectory
                      << "': " << ec.message() << std::endl;
            return;
        }

        if (!downloadDirExists || !downloadDirIsDirectory) {
            std::cerr << "Download monitor disabled. Invalid downloadDir: " << m_downloadDirectory << std::endl;
            return;
        }

        inotifyFd = WPEFramework::Core::inotify_init1(
            WPEFramework::Core::IN_CLOEXEC | WPEFramework::Core::IN_NONBLOCK);
        if (inotifyFd < 0) {
            std::cerr << "inotify_init1 failed: " << strerror(errno) << std::endl;
            return;
        }

        watchFd = WPEFramework::Core::inotify_add_watch(
            inotifyFd,
            m_downloadDirectory.c_str(),
            IN_CLOSE_WRITE | IN_MOVED_TO);
        if (watchFd < 0) {
            std::cerr << "inotify_add_watch failed: " << strerror(errno) << std::endl;
            close(inotifyFd);
            inotifyFd = -1;
            return;
        }

        std::cout << "Monitoring download directory for reference app packages: " << m_downloadDirectory << std::endl;

        auto processCandidateFile = [this](const fs::path& packagePath) {
            // Skip hidden files (those starting with a dot).
            const std::string fileName = packagePath.filename().string();
            if (!fileName.empty() && fileName[0] == '.') {
                return;
            }
            if (!m_stopDownloadMonitorThread && isReadyDownloadedFile(packagePath)) {
                if (!flushFileData(packagePath)) {
                    std::cerr << "Warning: failed to flush downloaded file before verification: "
                              << packagePath << " error=" << strerror(errno) << std::endl;
                }
                processDownloadedPackage(packagePath);
            }
        };

        auto enqueueCandidate = [&settleQueueMutex, &settleQueueCv, &settleQueue](
            const fs::path& packagePath,
            const std::chrono::milliseconds settleDelay) {
            const auto readyAt = std::chrono::steady_clock::now() + settleDelay;
            {
                std::lock_guard<std::mutex> lock(settleQueueMutex);
                settleQueue.emplace_back(packagePath, readyAt);
            }
            settleQueueCv.notify_one();
        };

        settleWorker = std::thread([this, &processCandidateFile, &settleQueueMutex, &settleQueueCv, &settleQueue, &stopSettleWorker]() {
            while (true) {
                fs::path nextPath;
                {
                    std::unique_lock<std::mutex> lock(settleQueueMutex);
                    settleQueueCv.wait(lock, [&]() {
                        return stopSettleWorker || !settleQueue.empty();
                    });

                    if (stopSettleWorker) {
                        break;
                    }

                    auto nextIt = settleQueue.begin();
                    for (auto it = settleQueue.begin(); it != settleQueue.end(); ++it) {
                        if (it->second < nextIt->second) {
                            nextIt = it;
                        }
                    }

                    const auto now = std::chrono::steady_clock::now();
                    if (nextIt->second > now) {
                        settleQueueCv.wait_until(lock, nextIt->second);
                        continue;
                    }

                    nextPath = nextIt->first;
                    settleQueue.erase(nextIt);
                }

                processCandidateFile(nextPath);
            }
        });

        // Optionally perform an initial sweep of the download directory
        // to catch any packages that were downloaded before this monitor started.
        const bool isInitialSweepEnabled = shouldRunInitialDownloadSweep();
        std::cout << "Initial download directory sweep is "
                  << (isInitialSweepEnabled ? "enabled" : "disabled")
                  << " (" << kInitialDownloadSweepEnvVar << ")" << std::endl;
        if (isInitialSweepEnabled) {
            std::error_code iterEc;
            fs::directory_iterator it(downloadDir, fs::directory_options::skip_permission_denied, iterEc);
            if (iterEc) {
                std::cerr << "Initial sweep failed while opening download directory: "
                          << iterEc.message() << std::endl;
            }
            fs::directory_iterator end;
            for (; it != end; it.increment(iterEc)) {
                if (iterEc) {
                    std::cerr << "Initial sweep failed while scanning download directory: "
                              << iterEc.message() << std::endl;
                    break;
                }

                const auto& entry = *it;

                std::error_code entryEc;
                if (!entry.is_regular_file(entryEc) || entryEc) {
                    continue;
                }

                // Skip hidden files (those starting with a dot).
                const std::string fileName = entry.path().filename().string();
                if (!fileName.empty() && fileName[0] == '.') {
                    continue;
                }

                enqueueCandidate(entry.path(), std::chrono::milliseconds::zero());
            }
        }

        // Start monitoring for new files in the download directory.
        constexpr size_t EVENT_SIZE = sizeof(WPEFramework::Core::inotify_event);
        constexpr size_t BUF_LEN = (EVENT_SIZE + NAME_MAX + 1) * 16;
        alignas(WPEFramework::Core::inotify_event) char buf[BUF_LEN];

        while (!m_stopDownloadMonitorThread) {
            struct pollfd pfd = { inotifyFd, POLLIN, 0 };
            const int ret = poll(&pfd, 1, 500);
            if (ret < 0) {
                if (errno == EINTR) continue;
                std::cerr << "inotify poll failed: " << strerror(errno) << std::endl;
                break;
            }
            if (ret == 0) continue; // timeout — check stop flag

            const ssize_t len = read(inotifyFd, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                std::cerr << "inotify read failed: " << strerror(errno) << std::endl;
                break;
            }
            if (len == 0) {
                std::cerr << "inotify read returned EOF; stopping monitor thread" << std::endl;
                break;
            }

            for (const char* ptr = buf; ptr < buf + len; ) {
                const auto* event = reinterpret_cast<const WPEFramework::Core::inotify_event*>(ptr);
                if (event->len > 0 && !(event->mask & IN_ISDIR)) {
                    // Skip hidden files (those starting with a dot).
                    if (event->name[0] != '.') {
                        enqueueCandidate(downloadDir / event->name, kDownloadedPackageSettleDelayMs);
                    }
                }
                ptr += EVENT_SIZE + event->len;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Unexpected exception in monitorDownloadDirectory: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unexpected non-standard exception in monitorDownloadDirectory" << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(settleQueueMutex);
        stopSettleWorker = true;
    }
    settleQueueCv.notify_all();
    if (settleWorker.joinable()) {
        settleWorker.join();
    }

    if (watchFd >= 0 && inotifyFd >= 0) {
        WPEFramework::Core::inotify_rm_watch(inotifyFd, watchFd);
    }
    if (inotifyFd >= 0) {
        close(inotifyFd);
    }
}

bool SceneSetApp::shouldRunInitialDownloadSweep() const {
    return isEnvFlagEnabled(kInitialDownloadSweepEnvVar, false);
}

#ifdef UNIT_TEST
void SceneSetApp::setMetadataExtractorForTesting(bool (*extractor)(const std::filesystem::path&, std::string&, std::string&)) {
    g_metadataExtractor = (extractor != nullptr) ? extractor : static_cast<MetadataExtractor>(&ralf_support::ExtractPackageMetadata);
}

void SceneSetApp::resetMetadataExtractorForTesting() {
    g_metadataExtractor = static_cast<MetadataExtractor>(&ralf_support::ExtractPackageMetadata);
}
#endif

bool SceneSetApp::processDownloadedPackage(const std::filesystem::path& packagePath) {
    std::string packageAppId;
    std::string packageVersion;

    if (!g_metadataExtractor(packagePath, packageAppId, packageVersion)) {
        std::cerr << "Failed to extract metadata from downloaded package: " << packagePath.filename() << std::endl;
        return false;
    }

    if (packageAppId != m_referenceAppId) {
        return false;
    }

    if (m_appLaunched.load()) {
        const std::string installedVersion = getInstalledReferenceAppVersion();
        if (!installedVersion.empty() && installedVersion == packageVersion) {
            std::cout << "Reference app '" << packageAppId
                      << "' is running with installed version " << installedVersion
                      << "; skipping staging for downloaded package " << packagePath.filename() << std::endl;
            return false;
        }
    }

    return movePackageToPreinstallDirectory(packagePath);
}

bool SceneSetApp::movePackageToPreinstallDirectory(const std::filesystem::path& sourceFile) {
    namespace fs = std::filesystem;
    const fs::path preinstallDir(m_preinstallDirectory);

    if (preinstallDir.empty()) {
        std::cerr << "appPreinstallDirectory is not configured. Cannot stage package." << std::endl;
        return false;
    }

    std::error_code ec;
    if (!fs::exists(sourceFile, ec) || ec) {
        std::cerr << "Source package disappeared before copy: " << sourceFile << std::endl;
        return false;
    }

    const bool preinstallDirExists = fs::exists(preinstallDir, ec);
    if (ec) {
        std::cerr << "Failed to validate preinstall directory '" << preinstallDir << "': "
                  << ec.message() << std::endl;
        return false;
    }

    if (preinstallDirExists && !fs::is_directory(preinstallDir, ec)) {
        if (ec) {
            std::cerr << "Failed to query preinstall directory type '" << preinstallDir << "': "
                      << ec.message() << std::endl;
        } else {
            std::cerr << "Configured appPreinstallDirectory is not a directory: " << preinstallDir << std::endl;
        }
        return false;
    }

    if (!preinstallDirExists) {
        try {
            fs::create_directories(preinstallDir);
        } catch (const fs::filesystem_error& e) {
            std::cerr << "Failed to create preinstall directory: " << e.what() << std::endl;
            return false;
        }
    }

    const fs::path destination = preinstallDir / sourceFile.filename();

    try {
        fs::rename(sourceFile, destination);
        if (!flushFileData(destination)) {
            std::cerr << "Warning: failed to flush staged package file: " << destination
                      << " error=" << strerror(errno) << std::endl;
        }
        std::cout << "Moved downloaded reference package to preinstall: " << destination << std::endl;
        return true;
    } catch (const fs::filesystem_error& e) {
        if (e.code() == std::errc::cross_device_link || e.code() == std::errc::file_exists) {
            try {
                std::error_code cleanupError;
                fs::copy_file(sourceFile, destination, fs::copy_options::overwrite_existing);
                fs::remove(sourceFile, cleanupError);
                if (cleanupError) {
                    std::cerr << "Copied package to preinstall but failed to remove source file: "
                              << sourceFile << " error=" << cleanupError.message() << std::endl;
                    return false;
                }

                if (!flushFileData(destination)) {
                    std::cerr << "Warning: failed to flush staged package file: " << destination
                              << " error=" << strerror(errno) << std::endl;
                }
                std::cout << "Copied downloaded package to preinstall (fallback path): "
                          << destination << "; removed source file: " << sourceFile << std::endl;
                return true;
            } catch (const fs::filesystem_error& e2) {
                std::cerr << "Failed copying package " << sourceFile << " to " << destination
                          << " after rename fallback: " << e2.what() << std::endl;
                return false;
            }
        }
        std::cerr << "Failed moving package " << sourceFile << " to " << destination << ": " << e.what() << std::endl;
        return false;
    }
}

std::string SceneSetApp::getInstalledReferenceAppVersion() const {
    if (m_referenceAppId.empty() || m_appManager == nullptr) {
        return "";
    }

    std::string installedApps;
    Core::hresult result = m_appManager->GetInstalledApps(installedApps);
    if (result != Core::ERROR_NONE || installedApps.empty()) {
        return "";
    }

    JsonArray apps;
    if (!apps.FromString(installedApps)) {
        return "";
    }

    JsonArray::Iterator index = apps.Elements();
    while (index.Next()) {
        const JsonValue& element = index.Current();
        if (element.Content() != JsonValue::type::OBJECT) {
            continue;
        }

        const JsonObject appObj = element.Object();
        if (!appObj.HasLabel("appId") || appObj["appId"].String() != m_referenceAppId) {
            continue;
        }

        if (appObj.HasLabel("version")) {
            return appObj["version"].String();
        }
        if (appObj.HasLabel("versionString")) {
            return appObj["versionString"].String();
        }
        return "";
    }

    return "";
}

bool SceneSetApp::fetchPluginConfigValue(const std::string& callsign, const std::string& configKey, std::string& value) const {
    value.clear();

    const std::string thunderAccessPath = getThunderAccessPath();
    auto shellClient = Core::ProxyType<RPC::CommunicatorClient>::Create(Core::NodeId(thunderAccessPath.c_str()));
    if (!shellClient.IsValid()) {
        return false;
    }

    PluginHost::IShell* controllerShell = shellClient->Open<PluginHost::IShell>(_T("Controller"));
    if (controllerShell == nullptr) {
        controllerShell = shellClient->Open<PluginHost::IShell>(_T("Controller.1"));
    }
    if (controllerShell == nullptr) {
        std::cerr << "Failed to open Controller shell for fetching config value" << std::endl;
        return false;
    }

    PluginHost::IShell* targetShell = controllerShell->QueryInterfaceByCallsign<PluginHost::IShell>(callsign.c_str());
    if (targetShell == nullptr) {
        std::cerr << "Failed to open shell for callsign: " << callsign << std::endl;
        controllerShell->Release();
        return false;
    }

    JsonObject configuration;
    const bool parsed = configuration.FromString(targetShell->ConfigLine());
    if (!parsed || !configuration.HasLabel(configKey.c_str()) || configuration[configKey.c_str()].Content() != JsonValue::type::STRING) {
        std::cerr << "Failed to parse configuration or config key not found or invalid type: " << configKey << std::endl;
        targetShell->Release();
        controllerShell->Release();
        return false;
    }

    value = configuration[configKey.c_str()].String();
    targetShell->Release();
    controllerShell->Release();
    return !value.empty();
}

std::string SceneSetApp::getThunderAccessPath() const {
    const char* thunderAccess = std::getenv("THUNDER_ACCESS");
    if (thunderAccess != nullptr && thunderAccess[0] != '\0') {
        return thunderAccess;
    }
    return m_comrpcPath;
}

void SceneSetApp::resolveDynamicDirectories() {
    std::string downloadDir;
    std::string preinstallDir;

    if (fetchPluginConfigValue(kAppPackageManagerCallsign, kPackageManagerDownloadDirKey, downloadDir)) {
        m_downloadDirectory = downloadDir;
        std::cout << "Updated downloadDir from AppPackageManager plugin config: " << m_downloadDirectory << std::endl;
    }

    if (fetchPluginConfigValue(m_preinstallCallsign, kPreinstallDirectoryKey, preinstallDir)) {
        m_preinstallDirectory = preinstallDir;
        std::cout << "Updated appPreinstallDirectory from PreinstallManager plugin config: " << m_preinstallDirectory << std::endl;
    }
}

std::string SceneSetApp::getSystemConfigPath() const {
    const char* configuredPath = std::getenv(kSceneSetSystemConfigEnvVar);
    if (configuredPath != nullptr && configuredPath[0] != '\0') {
        return configuredPath;
    }
    return SCENESET_SYSTEM_CONFIG_FILE;
}

bool SceneSetApp::loadSystemConfig(std::unordered_map<std::string, std::string>& values) const {
    values.clear();

#if !ENABLE_SYSTEM_CONFIG
    return false;
#else
    const std::string configPath = getSystemConfigPath();
    if (!parseSystemConfig(configPath, values)) {
        return false;
    }

#if ENABLE_CONFIG_OVERRIDE
    std::string overrideConfigPath = SCENESET_OVERRIDE_CONFIG_FILE;
    const char* configuredOverridePath = std::getenv(kSceneSetOverrideConfigEnvVar);
    if (configuredOverridePath != nullptr && configuredOverridePath[0] != '\0') {
        overrideConfigPath = configuredOverridePath;
    }

    std::unordered_map<std::string, std::string> overrideValues;
    if (parseSystemConfig(overrideConfigPath, overrideValues)) {
        for (const auto& [key, value] : overrideValues) {
            values[key] = value;
        }
        std::cout << "Applied override config from " << overrideConfigPath << std::endl;
    }
#endif

    return true;
#endif
}

void SceneSetApp::recordSceneSetStartTimestamp() {
    m_telemetryMetricsState.sceneSetStartTsMs = monotonicTimestampMs();
}

void SceneSetApp::recordPreinstallStartTimestamp() {
    m_telemetryMetricsState.preinstallStartTsMs = monotonicTimestampMs();
}

void SceneSetApp::recordPreinstallEndTimestamp() {
    m_telemetryMetricsState.preinstallEndTsMs = monotonicTimestampMs();
}

void SceneSetApp::recordLaunchRequestTimestamp() {
    m_telemetryMetricsState.lastLaunchRequestTsMs = monotonicTimestampMs();
    m_telemetryMetricsState.pendingActiveTelemetry = true;
}

void SceneSetApp::setLastTerminationNature(TerminationNature nature) {
    m_telemetryMetricsState.lastTerminationNature = static_cast<int>(nature);
}

const char* SceneSetApp::toTerminationNatureString(TerminationNature nature) {
    switch (nature) {
    case TerminationNature::CRASH:
        return "crash";
    case TerminationNature::INTENTIONAL_KILL:
        return "intentional_kill";
    case TerminationNature::NONE:
    default:
        return "none";
    }
}

void SceneSetApp::publishHomeAppActiveTelemetry(uint64_t activeTimestampMs) {
    const uint64_t sceneSetStartTsMs = m_telemetryMetricsState.sceneSetStartTsMs.load();
    const uint64_t preinstallStartTsMs = m_telemetryMetricsState.preinstallStartTsMs.load();
    const uint64_t preinstallEndTsMs = m_telemetryMetricsState.preinstallEndTsMs.load();
    const uint64_t launchRequestTsMs = m_telemetryMetricsState.lastLaunchRequestTsMs.load();

    const uint64_t totalStartToActiveMs =
        (sceneSetStartTsMs > 0 && activeTimestampMs >= sceneSetStartTsMs)
            ? (activeTimestampMs - sceneSetStartTsMs)
            : 0;
    const uint64_t preinstallDurationMs =
        (preinstallStartTsMs > 0 && preinstallEndTsMs >= preinstallStartTsMs)
            ? (preinstallEndTsMs - preinstallStartTsMs)
            : 0;
    const uint64_t launchToActiveMs =
        (launchRequestTsMs > 0 && activeTimestampMs >= launchRequestTsMs)
            ? (activeTimestampMs - launchRequestTsMs)
            : 0;

    const TerminationNature terminationNature = static_cast<TerminationNature>(m_telemetryMetricsState.lastTerminationNature.load());
    const uint32_t cumulativeRelaunchCount = m_telemetryMetricsState.cumulativeRelaunchCount.load();
    const bool isInitialLaunchTelemetry =
        (cumulativeRelaunchCount == 0 && terminationNature == TerminationNature::NONE);

    JsonObject telemetryPayload;
    telemetryPayload["appId"] = m_referenceAppId;
    telemetryPayload["launchToActiveMs"] = static_cast<uint32_t>(launchToActiveMs);
    // Keep cold-start timing metrics only on the first successful ACTIVE transition.
    // This avoids reusing startup/preinstall timings for later app restarts.
    if (isInitialLaunchTelemetry) {
        telemetryPayload["totalStartToActiveMs"] = static_cast<uint32_t>(totalStartToActiveMs);
        telemetryPayload["preinstallDurationMs"] = static_cast<uint32_t>(preinstallDurationMs);
    } else {
        // For relaunches, report only restart context fields and per-launch ACTIVE timing.
        telemetryPayload["cumulativeRelaunchCount"] = cumulativeRelaunchCount;
        telemetryPayload["terminationNature"] = toTerminationNatureString(terminationNature);
    }

    std::string payload;
    telemetryPayload.ToString(payload);
    if (!payload.empty()) {
        publishTelemetryMarker(kSceneSetHomeAppLaunchMarker, payload);
    }
}

void SceneSetApp::publishTelemetryMarker(const std::string& marker, const std::string& payload) {
#ifdef UNIT_TEST
    std::lock_guard<std::mutex> lock(m_lock);
    m_lastTelemetryMarker = marker;
    m_lastTelemetryPayload = payload;
#endif

#if SCENESET_TELEMETRY_METRICS_SUPPORT
    std::string markerBuffer = marker;
    std::string payloadBuffer = payload;
    t2_event_s(markerBuffer.data(), payloadBuffer.data());
#else
    (void)marker;
    (void)payload;
#endif
}

SceneSetApp::PreinstallManagerEventHandler::~PreinstallManagerEventHandler() {}

void SceneSetApp::PreinstallManagerEventHandler::OnAppInstallationStatus(const string &jsonresponse) {
    std::cout << "PreinstallManager OnAppInstallationStatus callback: " << jsonresponse << std::endl;
}

void SceneSetApp::PreinstallManagerEventHandler::OnPreinstallationComplete() {
    std::cout << "OnPreinstallationComplete received" << std::endl;
    SceneSetApp::getInstance().startPreinstallCompletionThread();
}

SceneSetApp::PackageInstallerEventHandler::~PackageInstallerEventHandler() {}

void SceneSetApp::PackageInstallerEventHandler::OnAppInstallationStatus(const string &jsonresponse) {
    std::cout << "PackageManager OnAppInstallationStatus: " << jsonresponse << std::endl;

    SceneSetApp& instance = SceneSetApp::getInstance();
    if (instance.m_startupPreinstallState.waitingForCompletion.load()) {
        instance.recordStartupPreinstallStatus(jsonresponse);
    }
}

uint32_t SceneSetApp::PreinstallManagerEventHandler::AddRef() const {
    return Core::ERROR_NONE;
}

uint32_t SceneSetApp::PreinstallManagerEventHandler::Release() const {
    return Core::ERROR_NONE;
}

uint32_t SceneSetApp::PackageInstallerEventHandler::AddRef() const {
    return Core::ERROR_NONE;
}

uint32_t SceneSetApp::PackageInstallerEventHandler::Release() const {
    return Core::ERROR_NONE;
}
