# SceneSet Testing Guide

## 1. Overview

SceneSet includes a comprehensive L1 (unit/integration) test suite using Google Test (gtest) and Google Mock (gmock). Tests are located in `Tests/L1Tests/` and cover core functionality, edge cases, and error handling.

### Test Infrastructure

| Component | Purpose |
|-----------|---------|
| `gtest` | Unit testing framework |
| `gmock` | Mocking framework for interface simulation |
| `SceneSetAppTestPeer` | Friend class for accessing private members |
| `ScopedEnvVarOverride` | RAII helper for environment variable manipulation |

---

## 2. Test File Organization

```
Tests/
└── L1Tests/
    ├── CMakeLists.txt              # Test build configuration
    └── tests/
        ├── test_SceneSet.cpp       # Main unit tests (~3100 lines)
        └── test_download_monitor.cpp # Download monitor specific tests
```

### Test Categories

| File | Coverage Area | Approximate Test Count |
|------|--------------|------------------------|
| `test_SceneSet.cpp` | Core SceneSetApp functionality | ~50+ tests |
| `test_download_monitor.cpp` | Download directory monitoring | ~5 tests |

---

## 3. Test Build Configuration

### CMakeLists.txt Highlights

```cmake
project(SceneSetL1Tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files - include the actual SceneSet implementation
set(TEST_SRC
    tests/test_SceneSet.cpp
    tests/test_download_monitor.cpp
    ../../src/SceneSet.cpp
    ../../src/RalfPackageSupport.cpp
)

# Create the test executable
add_executable(${TEST_EXECUTABLE} ${TEST_SRC})

# Enable UNIT_TEST macro for test hooks
target_compile_definitions(${TEST_EXECUTABLE} PRIVATE
    UNIT_TEST
    DAC_APP_CERT_PATH="/etc/rdk/certs"
    ENABLE_SYSTEM_CONFIG=1
)
```

### Key Build Definitions

| Definition | Purpose |
|------------|---------|
| `UNIT_TEST` | Enables test hooks and friend class access |
| `ENABLE_SYSTEM_CONFIG=1` | Enables system config parsing in tests |
| `DAC_APP_CERT_PATH` | Required compile-time certificate path |

---

## 4. Test Peer Class

The `SceneSetAppTestPeer` class provides test access to private members and methods of `SceneSetApp`.

### Class Definition (from `test_SceneSet.cpp`)

```cpp
class SceneSetAppTestPeer {
public:
    // Accessors
    static const std::string& GetReferenceAppId(const SceneSetApp& app);
    static bool GetAppLaunched(const SceneSetApp& app);
    static bool GetWaitingForStartupPreinstallCompletion(const SceneSetApp& app);
    static bool GetStartupPreinstallHasFailure(const SceneSetApp& app);
    
    // Mutators
    static void SetPreinstallDirectory(SceneSetApp& app, const std::string& dir);
    static void SetDownloadDirectory(SceneSetApp& app, const std::string& dir);
    static void SetAppLaunched(SceneSetApp& app, bool value);
    static void SetIsActive(SceneSetApp& app, bool value);
    static void SetFirmwareVersionFile(SceneSetApp& app, const std::string& path);
    static void SetLastFirmwareVersionMarker(SceneSetApp& app, const std::string& path);
    static void SetFactoryAppsCopiedMarker(SceneSetApp& app, const std::string& path);
    static void SetFactoryAppPath(SceneSetApp& app, const std::string& path);
    
    // Method wrappers
    static bool MovePackageToPreinstallDirectory(SceneSetApp& app, const fs::path& src);
    static bool ProcessDownloadedPackage(SceneSetApp& app, const fs::path& path);
    static bool ShouldRunInitialDownloadSweep(const SceneSetApp& app);
    static void CallRecordStartupPreinstallStatus(SceneSetApp& app, const std::string& json);
    static bool CallIsStartupPreinstallSucceed(const SceneSetApp& app);
    
    // Test hooks
    static void SetMetadataExtractorForTesting(
        bool (*extractor)(const fs::path&, std::string&, std::string&));
    static void ResetMetadataExtractorForTesting();
};
```

---

## 5. Test Utilities

### ScopedEnvVarOverride

RAII helper for temporarily setting environment variables:

```cpp
class ScopedEnvVarOverride {
public:
    ScopedEnvVarOverride(const char* name, const char* value)
        : m_name(name), m_hadOriginalValue(false) {
        const char* originalValue = std::getenv(name);
        if (originalValue != nullptr) {
            m_hadOriginalValue = true;
            m_originalValue = originalValue;
        }
        setenv(name, value, 1);
    }

    ~ScopedEnvVarOverride() {
        if (m_hadOriginalValue) {
            setenv(m_name.c_str(), m_originalValue.c_str(), 1);
        } else {
            unsetenv(m_name.c_str());
        }
    }
};
```

**Usage Example:**
```cpp
TEST(ConfigTest, EnvVarOverride) {
    ScopedEnvVarOverride override("SCENESET_INITIAL_DOWNLOAD_SWEEP", "1");
    // Test code that uses the environment variable
    // Original value restored when test exits
}
```

### MakeUniqueTempPath

Creates unique temporary paths for test isolation:

```cpp
std::filesystem::path MakeUniqueTempPath(const std::string& prefix) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::ostringstream name;
    name << prefix << "_" << now << "_" << tid;
    return std::filesystem::temp_directory_path() / name.str();
}
```

---

## 6. Test Coverage Areas

### 6.1 Preinstall Status Tracking Tests

```cpp
// Test: Recording installed package status
TEST(PreinstallStatusTest, RecordInstalledStatus) {
    SceneSetApp& app = SceneSetApp::getInstance();
    SceneSetAppTestPeer::CallResetStartupPreinstallStatusTracking(app);
    
    const std::string json = R"([{"packageId":"com.test.app","version":"1.0.0","state":"INSTALLED"}])";
    SceneSetAppTestPeer::CallRecordStartupPreinstallStatus(app, json);
    
    EXPECT_TRUE(SceneSetAppTestPeer::CallIsStartupPreinstallSucceed(app));
}

// Test: Recording failed package status
TEST(PreinstallStatusTest, RecordFailedStatus) {
    SceneSetApp& app = SceneSetApp::getInstance();
    SceneSetAppTestPeer::CallResetStartupPreinstallStatusTracking(app);
    
    const std::string json = R"([{"packageId":"com.test.app","version":"1.0.0","state":"FAILED"}])";
    SceneSetAppTestPeer::CallRecordStartupPreinstallStatus(app, json);
    
    EXPECT_FALSE(SceneSetAppTestPeer::CallIsStartupPreinstallSucceed(app));
}
```

### 6.2 Package Movement Tests

```cpp
TEST(PackageMovementTest, MovePackageSuccess) {
    const auto tempDir = MakeUniqueTempPath("preinstall_test");
    fs::create_directories(tempDir);
    
    SceneSetApp& app = SceneSetApp::getInstance();
    SceneSetAppTestPeer::SetPreinstallDirectory(app, tempDir.string());
    
    // Create test package file
    const auto sourceFile = tempDir / "source" / "test.ipk";
    fs::create_directories(sourceFile.parent_path());
    { std::ofstream f(sourceFile); f << "package content"; }
    
    // Test move
    EXPECT_TRUE(SceneSetAppTestPeer::MovePackageToPreinstallDirectory(app, sourceFile));
    EXPECT_TRUE(fs::exists(tempDir / "test.ipk"));
    EXPECT_FALSE(fs::exists(sourceFile));
    
    // Cleanup
    fs::remove_all(tempDir);
}
```

### 6.3 Download Monitor Tests

```cpp
TEST(DownloadMonitorTest, InitialSweepIgnoresHiddenFiles) {
    const auto downloadDir = MakeUniqueTempPath("download_monitor_test");
    fs::create_directories(downloadDir);

    // Create test files: mix of hidden and regular
    { std::ofstream f(downloadDir / ".hidden_file.ipk"); f << "hidden"; }
    { std::ofstream f(downloadDir / "package.ipk"); f << "regular"; }

    // Simulate initial sweep filtering logic
    const auto filtered = GetNonHiddenRegularFiles(downloadDir);

    // Verify only regular (non-hidden) files are returned
    EXPECT_EQ(filtered.size(), 1);
    EXPECT_EQ(filtered[0].filename().string(), "package.ipk");

    fs::remove_all(downloadDir);
}

TEST(DownloadMonitorTest, InitialSweepHandlesEmptyDirectory) {
    const auto downloadDir = MakeUniqueTempPath("download_monitor_empty_test");
    fs::create_directories(downloadDir);

    const auto filtered = GetNonHiddenRegularFiles(downloadDir);
    EXPECT_TRUE(filtered.empty());

    fs::remove_all(downloadDir);
}
```

### 6.4 Environment Variable Tests

```cpp
TEST(EnvVarTest, InitialSweepDisabledByDefault) {
    SceneSetApp& app = SceneSetApp::getInstance();
    // No environment variable set
    EXPECT_FALSE(SceneSetAppTestPeer::ShouldRunInitialDownloadSweep(app));
}

TEST(EnvVarTest, InitialSweepEnabledByEnvVar) {
    ScopedEnvVarOverride override("SCENESET_INITIAL_DOWNLOAD_SWEEP", "1");
    SceneSetApp& app = SceneSetApp::getInstance();
    EXPECT_TRUE(SceneSetAppTestPeer::ShouldRunInitialDownloadSweep(app));
}

TEST(EnvVarTest, InitialSweepAcceptsVariousValues) {
    const std::vector<std::string> enabledValues = {"1", "true", "yes", "on", "TRUE", "Yes"};
    for (const auto& value : enabledValues) {
        ScopedEnvVarOverride override("SCENESET_INITIAL_DOWNLOAD_SWEEP", value.c_str());
        SceneSetApp& app = SceneSetApp::getInstance();
        EXPECT_TRUE(SceneSetAppTestPeer::ShouldRunInitialDownloadSweep(app)) 
            << "Failed for value: " << value;
    }
}
```

### 6.5 Metadata Extractor Mock Tests

```cpp
// Mock metadata extractor for testing
static bool MockMetadataExtractor(
    const std::filesystem::path& path,
    std::string& appId,
    std::string& version) {
    appId = "com.test.referenceapp";
    version = "2.0.0";
    return true;
}

TEST(PackageProcessingTest, ProcessDownloadedPackageWithMock) {
    SceneSetAppTestPeer::SetMetadataExtractorForTesting(MockMetadataExtractor);
    
    // Test package processing with mocked metadata
    SceneSetApp& app = SceneSetApp::getInstance();
    // ... test code ...
    
    SceneSetAppTestPeer::ResetMetadataExtractorForTesting();
}
```

---

## 7. Running Tests

### Build Tests

```bash
# From repository root
cd Tests/L1Tests
mkdir build && cd build

cmake .. \
    -DCMAKE_PREFIX_PATH=/path/to/wpeframework \
    -DCMAKE_BUILD_TYPE=Debug

make -j$(nproc)
```

### Execute Tests

```bash
# Run all tests
./SceneSetL1TestsIN

# Run specific test suite
./SceneSetL1TestsIN --gtest_filter="DownloadMonitorTest.*"

# Run with verbose output
./SceneSetL1TestsIN --gtest_print_time=1

# Generate XML report
./SceneSetL1TestsIN --gtest_output=xml:test_results.xml
```

### Code Coverage

The test CMakeLists.txt enables coverage instrumentation for Debug builds:

```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${TEST_EXECUTABLE}
        PRIVATE
        --coverage
        -fprofile-arcs
        -ftest-coverage
    )
endif()
```

**Generate Coverage Report:**
```bash
# Build with coverage
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# Run tests
./SceneSetL1TestsIN

# Generate coverage report
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```

---

## 8. CI/CD Integration

### GitHub Workflow

**File:** `.github/workflows/SceneSet-L1-tests.yml`

The repository includes a GitHub Actions workflow for automated testing:

```yaml
name: SceneSet L1 Tests
on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build and Test
        run: |
          cd Tests/L1Tests
          mkdir build && cd build
          cmake ..
          make
          ./SceneSetL1TestsIN
```

---

## 9. Test Quality Analysis

### Current Coverage

| Component | Estimated Coverage | Notes |
|-----------|-------------------|-------|
| `SceneSetApp` core methods | High | Comprehensive test peer access |
| Preinstall status tracking | High | Multiple state scenarios tested |
| Download monitor | Medium | File filtering tested, inotify integration limited |
| RALF package support | Low | Requires actual libralf for full coverage |
| Event handlers | Low | Requires WPEFramework mocks |

### Missing Test Coverage

| Area | Reason | Suggested Approach |
|------|--------|-------------------|
| COMRPC initialization | Requires Thunder runtime | Integration tests on target |
| Event handler callbacks | Complex interface mocking | Add gmock-based interface mocks |
| Real inotify behavior | Timing-dependent | Use mock filesystem or longer tests |
| System config file parsing | File I/O | Use temp files in tests (partially done) |

### Recommended Additional Tests

1. **Factory Reset Flow**
   ```cpp
   TEST(FactoryResetTest, FirstBootCopiesFactoryApps) {
       // Setup: No marker file, factory apps exist
       // Action: Simulate startup
       // Verify: Apps copied, marker created only after a successful preinstall
   }
   ```

2. **Concurrent Thread Safety**
   ```cpp
   TEST(ThreadSafetyTest, ConcurrentLaunchThreadStarts) {
       // Start multiple launch threads rapidly
       // Verify no crashes or race conditions
   }
   ```

3. **Error Recovery**
   ```cpp
   TEST(ErrorRecoveryTest, HandlesMissingPreinstallDirectory) {
       // Setup: Non-existent preinstall directory
       // Action: Attempt package move
       // Verify: Directory created, move succeeds
   }
   ```

---

## 10. Beginner-to-Expert Learning Path

### Beginner Level

1. **Understand the test structure** — Read `test_download_monitor.cpp` first (simpler)
2. **Run existing tests** — Build and execute the test suite
3. **Read test peer class** — Understand how tests access private members

### Intermediate Level

1. **Add simple unit tests** — Test helper functions like `trimString()`, `isEnvFlagEnabled()`
2. **Use mocking** — Create mock metadata extractors
3. **Test edge cases** — Empty directories, missing files, invalid JSON

### Advanced Level

1. **Mock WPEFramework interfaces** — Create gmock mocks for `IAppManager`, `IPreinstallManager`
2. **Integration testing** — Test with actual Thunder on target device
3. **Performance testing** — Measure inotify event processing latency

---

## Next Steps

- [Architecture Overview](../docs/architecture.md) — System context for integration tests
- [Configuration Guide](../docs/configuration.md) — Build and runtime configuration options
