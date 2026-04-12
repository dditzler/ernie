#pragma once

// ============================================================
//  Firmware Version — vibration_xiao_c6
//  Bump these values before tagging a release in GitHub.
//  Format: MAJOR.MINOR.PATCH  (values 0-255 each)
//
//  Release workflow:
//    1. Increment the numbers below and update FW_VERSION_STR.
//    2. Commit: git add vibration_xiao_c6/src/version.h && git commit -m "chore: bump sensor-c6 to 1.x.x"
//    3. Tag:    git tag sensor-c6-v1.x.x && git push origin sensor-c6-v1.x.x
//    4. GitHub Actions builds the binary, publishes a Release,
//       and updates sensor-c6-version.json — device detects it on next wake.
// ============================================================

#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0

// Human-readable string printed to Serial on boot.
#define FW_VERSION_STR    "1.0.0"

// Numeric value for integer comparisons during OTA version checks.
// Encoded as 0x00MMNNPP (Major / Minor / Patch).
#define FW_VERSION_INT  ((uint32_t)(FW_VERSION_MAJOR) << 16 | \
                         (uint32_t)(FW_VERSION_MINOR) <<  8 | \
                         (uint32_t)(FW_VERSION_PATCH))
