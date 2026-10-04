# Spec: Preinstall Management

## Overview

At every boot, SceneSet triggers preinstallation of app bundles via PreinstallManager before launching the reference app. On the first boot (Factory Settings Reset / FSR), SceneSet first copies factory app bundles from a compile-time-configured source directory into the preinstall directory, then starts preinstall with force-install mode. On subsequent boots, it uses normal (version-aware) install mode.

---

## Behaviors

### 1. First Boot / Factory Settings Reset (FSR) Detection

First-boot detection is selected at build time via the `ENABLE_FIRMWARE_CHANGE_DETECTION` CMake option.

**Default (`ENABLE_FIRMWARE_CHANGE_DETECTION=OFF`) — marker-file strategy:**
- SceneSet detects a first boot by checking for the absence of a marker file at `/opt/persistent/.sceneset_factory_apps_copied`.
- If the marker file is absent, the boot is treated as an FSR.
- If the marker file is present, the boot is treated as a normal (non-FSR) boot.

**Opt-in (`ENABLE_FIRMWARE_CHANGE_DETECTION=ON`) — firmware-version strategy:**
- SceneSet detects a first boot by comparing the current firmware version against the firmware version recorded during the previous boot.
- The current firmware version is obtained from the `org.rdk.System` plugin via `ISystemServices::GetDownloadedFirmwareInfo` (`currentFWVersion`), falling back to the `imagename:` line of `/version.txt` when the plugin is unavailable.
- The previously recorded firmware version is read from `/opt/persistent/.sceneset_last_firmware_version`.
- If the current firmware version differs from the recorded version, or if no recorded version exists, the boot is treated as an FSR.
- If the current firmware version matches the recorded version, the boot is treated as a normal (non-FSR) boot.
- If the current firmware version cannot be determined, the boot is treated as an FSR.

---

### 2. Factory App Copying (FSR only)

On an FSR boot:

1. SceneSet copies all regular files from the compile-time `FACTORY_APP_PATH` directory into the configured preinstall directory.
2. Subdirectories and non-regular files within `FACTORY_APP_PATH` are skipped.
3. Existing files in the preinstall directory are overwritten.
4. SceneSet does **not** record the first boot here. The marker file (`ENABLE_FIRMWARE_CHANGE_DETECTION=OFF`) or the firmware version (`ON`) is recorded only after a successful preinstall (see Post-Preinstall Actions), so an interrupted or failed first boot re-triggers the first-boot flow on the next boot.

**Failure modes:**
- If `FACTORY_APP_PATH` does not exist, SceneSet logs an error and continues to the preinstall step anyway.
- Individual file copy failures are logged but do not stop the overall copy operation.
- If directory iteration fails, SceneSet logs an error and returns false, but still proceeds to the preinstall step.
- In all three cases the copy is reported as failed: with `ENABLE_FIRMWARE_CHANGE_DETECTION=OFF` the marker file is not created, so the next boot copies the factory apps again.

---

### 3. Starting Preinstall

After the factory app copying step (or on a normal boot, skipping it):

- SceneSet calls `PreinstallManager.StartPreinstall(forceInstall)`:
  - `forceInstall = true` on FSR (reinstall all packages regardless of version).
  - `forceInstall = false` on normal boot (only install if a newer version is present).
- If `StartPreinstall` returns an error, SceneSet logs the error and continues the startup flow immediately (skipping the preinstall completion wait, not deleting preinstall files).

---

### 4. Waiting for Preinstall Completion

SceneSet waits for `PreinstallManager.OnPreinstallationComplete` before continuing startup. During this wait, SceneSet monitors per-package installation status events from `AppPackageManager.OnAppInstallationStatus`.

Each package status event is parsed as a JSON array of objects with `packageId`, `state`, and optional `version` fields. A package is considered to have failed if its `state` is not `INSTALLED` or `INSTALLING`.

If SceneSet cannot register for `AppPackageManager` status events, the preinstall result cannot be verified and is treated as failed (see Post-Preinstall Actions).

If any package reports a failure state before `OnPreinstallationComplete`, the preinstall phase is recorded as failed.

---

### 5. Post-Preinstall Actions

When `OnPreinstallationComplete` fires:

| Preinstall result | Action |
|---|---|
| All packages succeeded (`INSTALLED` or `INSTALLING`) | Clean up the preinstall directory; `sync()` so the installed packages are on disk; when `ENABLE_FIRMWARE_CHANGE_DETECTION=ON`, record the current firmware version to `/opt/persistent/.sceneset_last_firmware_version` (skipped if the version is unavailable); when `OFF` and this was a first boot whose factory copy succeeded, create the marker file `/opt/persistent/.sceneset_factory_apps_copied` and fsync it (an error is logged if it cannot be created); then check if reference app is installed and launch it |
| Any package failed | Preserve the preinstall directory (for retry on next boot); do **not** record the firmware version or create the marker file, so the next boot re-runs the first-boot flow; still check if app is installed and launch it |

**Failure mode:** If `OnPreinstallationComplete` is never received (e.g. PreinstallManager crashes), the startup flow stalls. SceneSet does not have a timeout or fallback for this case.

---

### 6. Preinstall Directory Cleanup

When cleanup is triggered:

- All files and subdirectories within the preinstall directory are removed.
- Individual removal failures are logged but do not stop the cleanup operation.
- If the preinstall directory does not exist, cleanup is a no-op.

---

### 7. Preinstall Directory Resolution

The preinstall directory is resolved in the following order of priority:

1. `preinstallLocation` from `/etc/sceneset.conf`, when present with a non-empty absolute path.
2. Dynamically fetched from `PreinstallManager` plugin config key `appPreinstallDirectory` at startup via the Thunder Controller.
3. Compile-time `APP_PREINSTALL_DIRECTORY` CMake variable (fallback if dynamic lookup returns empty).
4. If all sources are unavailable or unusable after initialization, SceneSet fails to start.

---

## Constraints

- SceneSet does not implement a retry mechanism for preinstall failures beyond preserving files for the next boot.
- There is no timeout on the wait for `OnPreinstallationComplete`.
- Factory app copying runs on every first boot until one completes (successful copy and preinstall); after that it is not triggered again (controlled by the marker file).
- `FACTORY_APP_PATH` must be set at build time if factory app copying is required; it cannot be configured at runtime.
- If `FACTORY_APP_PATH` is set at build time, `APP_PREINSTALL_DIRECTORY` must also be set (enforced at CMake configure time).
