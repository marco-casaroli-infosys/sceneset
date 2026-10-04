/**
* If not stated otherwise in this file or this component's LICENSE
* file the following copyright and licenses apply:
*
* Copyright 2025 RDK Management
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
**/

// First-boot marker lifecycle with the marker strategy (ENABLE_FIRMWARE_CHANGE_DETECTION=0).

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "SceneSet.h"

#if ENABLE_FIRMWARE_CHANGE_DETECTION
#error "test_SceneSetMarker.cpp must be built with ENABLE_FIRMWARE_CHANGE_DETECTION=0"
#endif

class SceneSetAppTestPeer {
public:
    static void SetFactoryAppPath(SceneSetApp& app, const std::string& path) { app.m_factoryAppPathOverride = path; }
    static void SetFactoryAppsCopiedMarker(SceneSetApp& app, const std::string& path) { app.m_factoryAppsCopiedMarkerOverride = path; }
    static void SetPreinstallDirectory(SceneSetApp& app, const std::string& path) { app.m_preinstallDirectory = path; }
    static void PrepareFactoryAppsForFirstBoot(SceneSetApp& app) { app.prepareFactoryAppsForFirstBoot(); }
    static void CompletePreinstall(SceneSetApp& app, bool failed) {
        app.m_startupPreinstallState.hasFailure = failed;
        app.m_startupPreinstallState.waitingForCompletion = true;
        app.m_isActive = true;
        app.completeStartupAfterPreinstall();
    }
    // Startup path: begin status tracking, then the completion notification arrives with no package status events.
    static void CompletePreinstallWithoutStatusEvents(SceneSetApp& app, bool statusEventsRegistered) {
        app.beginStartupPreinstallStatusTracking(statusEventsRegistered);
        app.m_startupPreinstallState.waitingForCompletion = true;
        app.m_isActive = true;
        app.completeStartupAfterPreinstall();
    }
};

namespace {
std::filesystem::path MakeUniqueTempPath(const std::string& prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::ostringstream name;
    name << prefix << "_" << now << "_" << tid;
    return std::filesystem::temp_directory_path() / name.str();
}
}

class SceneSetMarkerTest : public ::testing::Test {
protected:
    std::filesystem::path factoryDir = MakeUniqueTempPath("sceneset_marker_factory");
    std::filesystem::path preinstallDir = MakeUniqueTempPath("sceneset_marker_preinstall");
    std::filesystem::path marker = MakeUniqueTempPath("sceneset_marker_file");

    void SetUp() override {
        setenv("THUNDER_ACCESS", "/tmp/communicator", 1);
        setenv("SCENESET_DEFAULT_APPNAME", "TestApp", 1);
        std::filesystem::create_directories(factoryDir);
        std::ofstream(factoryDir / "app.bolt") << "data";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(factoryDir, ec);
        std::filesystem::remove_all(preinstallDir, ec);
        std::filesystem::remove(marker, ec);
        unsetenv("THUNDER_ACCESS");
        unsetenv("SCENESET_DEFAULT_APPNAME");
    }

    void Configure(SceneSetApp& app, const std::filesystem::path& factory) {
        SceneSetAppTestPeer::SetFactoryAppPath(app, factory.string());
        SceneSetAppTestPeer::SetFactoryAppsCopiedMarker(app, marker.string());
        SceneSetAppTestPeer::SetPreinstallDirectory(app, preinstallDir.string());
    }
};

// The copy alone must not record the first boot: an interrupted preinstall has to be retried.
TEST_F(SceneSetMarkerTest, CopyAloneLeavesNoMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    EXPECT_TRUE(std::filesystem::exists(preinstallDir / "app.bolt"));
    EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(SceneSetMarkerTest, CopyAndSuccessfulPreinstallCreateMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    SceneSetAppTestPeer::CompletePreinstall(app, false);
    EXPECT_TRUE(std::filesystem::exists(marker));
}

TEST_F(SceneSetMarkerTest, FailedPreinstallLeavesNoMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    SceneSetAppTestPeer::CompletePreinstall(app, true);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(SceneSetMarkerTest, FailedCopyLeavesNoMarker) {
    SceneSetApp app;
    Configure(app, MakeUniqueTempPath("sceneset_marker_missing_factory"));
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    SceneSetAppTestPeer::CompletePreinstall(app, false);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

// One bundle fails to copy (its destination is a directory): no marker, so the next boot retries the copy.
TEST_F(SceneSetMarkerTest, FailedBundleCopyLeavesNoMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    std::ofstream(factoryDir / "other.bolt") << "data";
    std::filesystem::create_directories(preinstallDir / "other.bolt");
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    EXPECT_TRUE(std::filesystem::exists(preinstallDir / "app.bolt"));
    SceneSetAppTestPeer::CompletePreinstall(app, false);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

// Normal boot (no factory copy): completing the preinstall must not write the marker.
TEST_F(SceneSetMarkerTest, NormalBootLeavesNoMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::CompletePreinstall(app, false);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

// PackageManager status registration failed: the preinstall result cannot be verified, so no marker.
TEST_F(SceneSetMarkerTest, UnregisteredStatusEventsLeaveNoMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    SceneSetAppTestPeer::CompletePreinstallWithoutStatusEvents(app, false);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

TEST_F(SceneSetMarkerTest, RegisteredStatusEventsWithoutFailureCreateMarker) {
    SceneSetApp app;
    Configure(app, factoryDir);
    SceneSetAppTestPeer::PrepareFactoryAppsForFirstBoot(app);
    SceneSetAppTestPeer::CompletePreinstallWithoutStatusEvents(app, true);
    EXPECT_TRUE(std::filesystem::exists(marker));
}
