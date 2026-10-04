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

#ifndef SCENESET_H
#define SCENESET_H

#include <iostream>
#include <memory>
#include <thread>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <systemd/sd-daemon.h>

#ifndef MODULE_NAME
#define MODULE_NAME Sceneset
#endif

#include <WPEFramework/com/com.h>
#include <WPEFramework/core/core.h>
#include "WPEFramework/interfaces/IAppPackageManager.h"
#include "WPEFramework/interfaces/IAppManager.h"
#include "WPEFramework/interfaces/IPreinstallManager.h"

using namespace std;
using namespace WPEFramework;

#ifdef UNIT_TEST
class SceneSetAppTestPeer;
#endif

class SceneSetApp {
public:
    SceneSetApp();
    ~SceneSetApp();

    bool initialize();
    bool registerForAppEvents();
    bool registerForPreinstallEvents();
    bool registerForPackageInstallerEvents();
    bool unRegisterForAppEvents();
    bool unRegisterForPreinstallEvents();
    bool unRegisterForPackageInstallerEvents();
    void releaseComInterfaces();
    bool launchDefaultApp();
    bool killReferenceApp();
    bool startPreinstall(bool forceInstall);
    bool isReferenceAppInstalled();
    void checkAndLaunchIfAlreadyInstalled();
    bool isFactoryAppsCopied();
    void markFactoryAppsCopied();
    bool isFirmwareChanged() const;
    std::string readCurrentFirmwareVersionFromSystem() const;
    std::string readCurrentFirmwareVersion() const;
    std::string readLastBootFirmwareVersion() const;
    void storeCurrentFirmwareVersion();
    bool copyFactoryAppsToPreinstall();
    void prepareFactoryAppsForFirstBoot();
    void cleanupPreinstallFolder();
    void waitForTermSignal();
    static void handleTerminationSignal(int signal);
    void onTerminate();
    static SceneSetApp& getInstance();
    void run();

private:
#ifdef UNIT_TEST
    friend class SceneSetAppTestPeer;
#endif

    enum class TerminationNature : int {
        NONE = 0,
        CRASH = 1,
        INTENTIONAL_KILL = 2,
    };

    struct StartupPreinstallState {
        std::mutex completionThreadMutex;
        std::atomic<bool> waitingForCompletion { false };
        std::atomic<bool> hasFailure { false };
        std::atomic<bool> markFactoryAppsCopiedOnSuccess { false };
    };

    struct TelemetryMetricsState {
        std::atomic<uint64_t> sceneSetStartTsMs { 0 };
        std::atomic<uint64_t> preinstallStartTsMs { 0 };
        std::atomic<uint64_t> preinstallEndTsMs { 0 };
        std::atomic<uint64_t> lastLaunchRequestTsMs { 0 };
        std::atomic<bool> pendingActiveTelemetry { false };
        std::atomic<uint32_t> cumulativeRelaunchCount { 0 };
        std::atomic<int> lastTerminationNature { static_cast<int>(TerminationNature::NONE) };
    };

#ifdef UNIT_TEST
    std::string m_lastTelemetryMarker;
    std::string m_lastTelemetryPayload;
#endif

    std::atomic<bool> m_isActive;
    std::mutex m_lock;
    Exchange::IAppManager *m_appManager;
    Exchange::IPreinstallManager *m_preinstallManager;
    Exchange::IPackageInstaller *m_packageInstaller;
    std::shared_ptr<WPEFramework::Exchange::IAppManager::INotification> m_appManagerEventHandler;
    std::shared_ptr<WPEFramework::Exchange::IPreinstallManager::INotification> m_preinstallManagerEventHandler;
    std::shared_ptr<WPEFramework::Exchange::IPackageInstaller::INotification> m_packageInstallerEventHandler;
    std::string m_appmgrCallsign, m_preinstallCallsign;
    std::string m_referenceAppId;
    std::string m_comrpcPath;
    std::string m_downloadDirectory;
    std::string m_preinstallDirectory;

    std::unique_ptr<std::thread> m_launchThread;
    std::atomic<bool> m_stopLaunchThread;
    std::atomic<bool> m_appLaunched;
    std::atomic<bool> m_pendingRestart;
    std::mutex m_launchThreadMutex;
    std::unique_ptr<std::thread> m_downloadMonitorThread;
    std::atomic<bool> m_stopDownloadMonitorThread;
    std::mutex m_downloadMonitorMutex;
    std::unique_ptr<std::thread> m_preinstallCompletionThread;
    StartupPreinstallState m_startupPreinstallState;
    TelemetryMetricsState m_telemetryMetricsState;

    void stopCurrentLaunchThread();
    void startLaunchThread();
    void startDownloadMonitorThread();
    void stopDownloadMonitorThread();
    void startPreinstallCompletionThread();
    void stopPreinstallCompletionThread();
    void completeStartupAfterPreinstall();
    void resetStartupPreinstallStatusTracking();
    void beginStartupPreinstallStatusTracking(bool statusEventsRegistered);
    void recordStartupPreinstallStatus(const std::string& jsonresponse);
    bool isStartupPreinstallSucceed() const;
    void monitorDownloadDirectory();
    bool shouldRunInitialDownloadSweep() const;
    bool processDownloadedPackage(const std::filesystem::path& packagePath);
    bool movePackageToPreinstallDirectory(const std::filesystem::path& sourceFile);
#ifdef UNIT_TEST
    static void setMetadataExtractorForTesting(bool (*extractor)(const std::filesystem::path&, std::string&, std::string&));
    static void resetMetadataExtractorForTesting();
    std::string m_factoryAppsCopiedMarkerOverride;
    std::string m_firmwareVersionFileOverride;
    std::string m_lastFirmwareVersionMarkerOverride;
    std::string m_factoryAppPathOverride;
#endif
    std::string getInstalledReferenceAppVersion() const;
    std::string getThunderAccessPath() const;
    bool fetchPluginConfigValue(const std::string& callsign, const std::string& configKey, std::string& value) const;
    void resolveDynamicDirectories();
    std::string getSystemConfigPath() const;
    bool loadSystemConfig(std::unordered_map<std::string, std::string>& values) const;
    void publishTelemetryMarker(const std::string& marker, const std::string& payload);
    void publishHomeAppActiveTelemetry(uint64_t activeTimestampMs);
    void recordSceneSetStartTimestamp();
    void recordPreinstallStartTimestamp();
    void recordPreinstallEndTimestamp();
    void recordLaunchRequestTimestamp();
    void setLastTerminationNature(TerminationNature nature);
    static const char* toTerminationNatureString(TerminationNature nature);

    class AppManagerEventHandler : public Exchange::IAppManager::INotification {
    public:
        ~AppManagerEventHandler();
        void OnAppInstalled(const string &appId, const string &version) override;
        void OnAppUninstalled(const string &appId) override;
        void OnAppLifecycleStateChanged(const string &appId, const string &appInstanceId, const Exchange::IAppManager::AppLifecycleState newState, const Exchange::IAppManager::AppLifecycleState oldState, const Exchange::IAppManager::AppErrorReason errorReason) override;
        void OnAppLaunchRequest(const string &appId, const string &intent, const string &source) override;
        const char* getAppStateString(const Exchange::IAppManager::AppLifecycleState state);
        void OnAppUnloaded(const string &appId, const string &appInstanceId) override;
        uint32_t AddRef() const override;
        uint32_t Release() const override;
        BEGIN_INTERFACE_MAP(AppManagerEventHandler)
        INTERFACE_ENTRY(Exchange::IAppManager::INotification)
        END_INTERFACE_MAP
    };

    class PreinstallManagerEventHandler : public Exchange::IPreinstallManager::INotification {
    public:
        ~PreinstallManagerEventHandler();
        void OnAppInstallationStatus(const string &jsonresponse) override;
        void OnPreinstallationComplete() override;
        uint32_t AddRef() const override;
        uint32_t Release() const override;
        BEGIN_INTERFACE_MAP(PreinstallManagerEventHandler)
        INTERFACE_ENTRY(Exchange::IPreinstallManager::INotification)
        END_INTERFACE_MAP
    };

    class PackageInstallerEventHandler : public Exchange::IPackageInstaller::INotification {
    public:
        ~PackageInstallerEventHandler();
        void OnAppInstallationStatus(const string &jsonresponse) override;
        uint32_t AddRef() const override;
        uint32_t Release() const override;
        BEGIN_INTERFACE_MAP(PackageInstallerEventHandler)
        INTERFACE_ENTRY(Exchange::IPackageInstaller::INotification)
        END_INTERFACE_MAP
    };
};

#endif // SCENESET_H
