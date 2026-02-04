# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-01-15
Version: 1.5.0

## Rule API

### General Rule Configuration

Get or set the global rule enablement status.

**Path:** `.../vigil/rule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "bRuleEnabled": true, // Boolean, enable or disable the evaluation of all rules, evalute precedence: schedule rule > record rule
    "dWriterConfig": {
        "sFormat": "MP4", // String, case-sensitive, one of: "MP4", "JPG", "RAW"
        "iIntervalMs": 0 // Integer, the delay between recordings in milliseconds including the write duration; 0 means continuous recording
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

### Rule Info

Get the information about the rule system.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "dLastRuleEvent": { // Optional, present only if a finished event exists, updated on current event finished
        "sStatus": "COMPLETED", // String, write event status, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN"
        "iTimestamp": 1735574000 // Integer, Unix timestamp in milliseconds
    },
    "dLastRuleEventOwner": { // Optional, present and update when a rule evaluated as true and the current event is finished
        "sRuleType": "TIMER", // String, rule type
        "sRuleID": "Hourly Timer", // String, rule ID
        "iTimestamp": 1735574400 // Integer, Unix timestamp in milliseconds (should be same as the corresponding timestamp of its event)
    },
    "dCurrentRuleEvent": { // Alternative to dLastRuleEvent, present only if an active event exists
        "sStatus": "WRITING",
        "iTimestamp": 1735574400
    },
    "dCurrentRuleEventOwner": { // Alternative to dLastRuleEventOwner, present only if an active event exists
        "sRuleType": "TIMER",
        "sRuleID": "Hourly Timer",
        "iTimestamp": 1735574400
    },
    "dAvailableGPIOs": { // Object, available GPIO pins
        "GPIO_01": { // Key: String, GPIO pin name
            "iNum": 1, // Integer, GPIO number
            "sState": "FLOATING", // String (optional), one of: "DISABLED", "FLOATING", "PULL_UP", "PULL_DOWN", null if unable to get the state of the pin
            "lCapabilities": ["FLOATING", "PULL_UP", "PULL_DOWN"], // Array of all available configurable input modes, all possible values already listed here, not for the purpose of peripheral circuit
            "sLevel": "HIGH" // String (optional), current level of the pin, one of: "HIGH", "LOW", null if unable to get the level of the pin
        },
        // ... More GPIO pins
    },
    "dAvailableTTYs": { // Object, available TTY devices
        "ttyS0": { // Key: String, TTY name
            "sSocketPath": "/tmp/ttyS0.sock", // String, Unix socket path for inter-process communication
            "iBufferSize": 1024 // Integer, buffer size in bytes, suggested maximum command length
        },
        // ... More TTY devices
    },
    "bMediaPaused": false, // Boolean, whether media capture is currently paused (e.g., due to configuration update in underlying encoder system)
    "bVideoClipLengthSeconds": 60 // Integer, target video clip length in seconds (estimated, may inaccurate because the GOP length varies)
}
```

### Schedule Rule Configuration

Get or set the schedule rule configuration.

**Path:** `.../vigil/rule/schedule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "bEnabled": true, // Boolean, enable or disable schedule rule, the schedule rule always evaluated as true when disabled
    "lActiveWeekdays": [ // Array of time ranges for active recording periods, evaluated in local time
        {
            "sStart": "Mon 09:00:00", // String, format: "Day HH:MM:SS", case-sensitive short weekday name with ISO 8601 time, (Day: Sun, Mon, Tue, Wed, Thu, Fri, Sat)
            "sEnd": "Mon 18:00:00" // String, format: "Day HH:MM:SS", Hour 24:00:00 is valid for end-of-day
        },
        // ... More time ranges, e.g., full week: {"sStart": "Mon 00:00:00", "sEnd": "Sun 24:00:00"}
    ],
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

### Record Rule Configuration

Get or set the record rule (trigger) configuration.

**Path:** `.../vigil/rule/record/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

The payload depends on the `sCurrentSelected` field.

#### Inference Set Rule

```json
{
    "sCurrentSelected": "INFERENCE_SET", // String, one of: "INFERENCE_SET", "TIMER", "GPIO", "TTY", "HTTP"
    "lInferenceSet": [
        {
            "sID": "person_detection", // String, rule ID (uniqueness not enforced but recommended)
            "iDebounceTimes": 3, // Integer, number of consecutive frames to confirm the activation
            "lConfidenceFilter": [0.5, 1.0], // Array of number [min, max], confidence range to filter, configurable range between 0.0 and 1.0 for each part
            "lClassFilter": [0, 1], // Array of integers, class IDs to filter
            "lRegionFilter": [ // Optional, list of regions to filter, empty list means no region filtering
                {
                    "lPolygon": [ // Array of polygons, each polygon is a list of points sorted in clockwise or counter-clockwise order
                        [0.0, 0.0], [0.5, 0.0], [0.5, 0.5], [0.0, 0.5] // List of [x, y] points (0.0-1.0), origin at top-left corner
                    ]
                },
                // ... More regions, all regions are ORed together
            ]
        },
        // ... More inference set rules, all rules are ORed together
    ],
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

#### Timer Rule

```json
{
    "sCurrentSelected": "TIMER",
    "dTimer": {
        "iIntervalSeconds": 60 // Integer, trigger interval in seconds, must be >= 0, activation will be ignored while previous event is still writing
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

#### GPIO Rule

```json
{
    "sCurrentSelected": "GPIO",
    "dGPIO": {
        "sName": "GPIO_01", // String, GPIO pin name (or provide "iNum" instead)
        "sState": "FLOATING", // String, GPIO pin state, one of: "DISABLED", "FLOATING", "PULL_UP", "PULL_DOWN"
        "sSignal": "HIGH", // String, GPIO pin signal to trigger recording, one of: "HIGH", "LOW", "RISING", "FALLING", activation will be ignored while previous event is still writing
        "iDebounceDurationMs": 100 // Integer, debounce duration in milliseconds, must be >= 0
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

#### TTY Rule

```json
{
    "sCurrentSelected": "TTY",
    "dTTY": {
        "sName": "ttyS0", // String, TTY name
        "sCommand": "RECORD" // String, non-empty command string to activate recording, activation will be ignored while previous event is still writing
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

#### HTTP Rule

The HTTP rule requires no additional configuration, select it to enable external trigger via the HTTP rule activation API.

```json
{
    "sCurrentSelected": "HTTP",
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

### HTTP Rule Activation

Trigger the currently selected HTTP rule (no payload required).

**Path:** `.../vigil/rule/http_rule/activate`

**Method:** `POST`

**Response Body:**

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

Notes:

- Returns error `EPERM` if the rule system is disabled.
- Returns error `EPERM` if the schedule rule evaluates to false.
- Returns error `EPERM` if the current selected rule is not `HTTP`.
- Returns error `EBUSY` if the storage system is not ready for writing.
- Returns error `EBUSY` if a pending activation request is still being processed.
- Returns error `EBUSY` if a previous recording event is still writing.

## Storage API

### Storage Configuration

Get or set the storage configuration.

**Path:** `.../vigil/storage/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "dSelectSlotToEnable": { // Object, set to null to disable enabled slots or not enable any slot
        "sByDevPath": "/dev/sda1", // String, device path to filter the target enabled slot, empty string to skip this filter
        "sByUUID": "550e8400-e29b-41d4-a716-446655440000" // String, UUID to filter the target enabled slot, empty string to skip this filter
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

### Storage Status

Get the current status of all storage slots.

**Path:** `.../vigil/storage/status`

**Method:** `GET`

**Response Body:**

```json
{
    "lSlots": [
        {
            "sDevPath": "/dev/sda1", // String, device path of the physical storage partition or volume
            "sMountPath": "/mnt/sda1", // String, target mount path, always non-empty, but the actual mount may not exist if eState < MOUNTED
            "bRemovable": true, // Boolean, whether the storage is removable
            "bInternal": false, // Boolean, whether the storage is internal partition
            "sLabel": "MyDrive", // String (optional), volume label, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "sUUID": "550e8400-e29b-41d4-a716-446655440000", // String (optional), volume UUID, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "sType": "ext4", // String (optional), filesystem type, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "bSelected": true, // Boolean, whether this slot is the target enabled slot according to current configuration
            "bEnabled": true, // Boolean, whether this slot is currently enabled
            "bWriting": false, // Boolean, whether this slot currently has activate writing tasks (e.g., recording videos, writing images)
            "bRotating": false, // Boolean, whether this slot is currently performing quota rotation
            "eState": 8, // Integer, states enum of current slot, values (1: ERROR, 2: NOT_FORMATTED_OR_FORMAT_UNSUPPORTED, 3: FORMATTING, 4: NOT_MOUNTED, 5: MOUNTED, 6: CONFIGURED, 7: INDEXING, 8: READY)
            "sState": "READY", // String, human-readable state ("ERROR", "NOT_FORMATTED_OR_FORMAT_UNSUPPORTED", "FORMATTING", "NOT_MOUNTED", "MOUNTED", "CONFIGURED", "INDEXING", "READY")
            "iStatsSizeBytes": 1000000000, // Integer, total size of the partition, not accurate if eState < MOUNTED
            "iStatsFreeBytes": 500000000, // Integer, free space of the partition, not accurate if eState < MOUNTED
            "iQuotaMinimumRecommendBytes": 200000000, // Integer, minimum recommended quota limit, may not work well if quota limit is set below this value
            "iQuotaPreservedBytes": 0, // Integer, space preserved for system use of the partition, the size of quota limit exceeds (iStatsSizeBytes - iQuotaPreservedBytes) may not be fully usable
            "iQuotaUsedBytes": 10000000, // Integer (optional), current used space under quota, null if eState < CONFIGURED, not accurate if eState < READY
            "iQuotaLimitBytes": 100000000, // Integer, configured quota limit, -1 means no limit, not accurate or uses the default if eState < CONFIGURED
            "bQuotaRotate": true // Boolean, whether quota rotation is enabled, not accurate or uses the default if eState < CONFIGURED
        },
        // ... More storage slots, dynamically updated
    ],
    "sDataDirName": "DCIM" // String, name of the record data directory under each storage mount point
}
```

Notes:

- The storage status is dynamically updated; clients should poll periodically to get the latest status.
- The actual used quota space (`iQuotaUsedBytes`) may be slightly larger than the sum of recorded files due to filesystem overhead.
- The `iQuotaPreservedBytes` is reserved to ensure system stability and prevent the filesystem from being completely filled.
- The `iQuotaMinimumRecommendBytes` is a guideline; setting the quota limit below this value may lead to suboptimal performance or not able to function properly.
- The quota limitation and rotation settings take effect only when the storage slot is in `CONFIGURED` or higher state.
- The quota limitation and rotation settings are stored on a per-slot basis, changing the settings for one slot does not affect others.
- Not the entire quota limit is used, e.g. if the the remaining free space on the storage device is less than the `iQuotaLimitBytes` - `iQuotaUsedBytes`, the actual usable space is limited by the remaining free space on the device, and if the quota rotation is enabled, the system will attempt to free up space by deleting the oldest files when the usable space is exhausted.
- The storage system automatically handles mounting, unmounting, and formatting of the storage slots as needed.
- The storage system ensures that only one storage slot is enabled at a time, enabling a new slot will disable the previously enabled slot.
- Modifying the large a mount of files directly under the record data directory of an enabled storage slot may interfere with the normal operation of the storage system, it is recommended to use the Storage Control API for file removal.
- The storage system may take some time to transition between states, clients should monitor the `eState` field to determine when the storage slot is ready for use.

### Storage Control

Perform an action on the storage system.

**Path:** `.../vigil/storage/control`

**Method:** `POST`

**Request Payload:**

```json
{
    "sAction": "FREE_UP", // String, one of: "FORMAT", "FREE_UP", "EJECT", "CONFIG", "RELAY", "RELAY_STATUS", "UNRELAY", "REMOVE_FILES_OR_DIRECTORIES"
    "sSlotDevPath": "/dev/sda1", // String, device path of the slot
    "dSlotConfig": { // Object, required only for "CONFIG" action
        "iQuotaLimitBytes": 1073741824, // Integer, quota limit in bytes, 0 means no limit
        "bQuotaRotate": true // Boolean, whether to enable quota rotation
    },
    "lFilesOrDirectoriesToRemove": [ // List, required only for "REMOVE_FILES_OR_DIRECTORIES" action
        "DCIM/20251016/event.mp4", // String, relative path under the record data directory, the sanitized path must still be under the record data directory, otherwise error may returned
        // ... More files or directories to remove
    ],
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

**Response Body:**

The response depends on the `sAction`.

For `RELAY` and `RELAY_STATUS` actions:

```json
{
    "dRelayStatus": {
        "iRelayTimeoutRemain": 25,
        "iRelayTimeout": 300,
        "sRelayDirectory": "550e8400-e29b-41d4-a716-446655440000"
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

For `REMOVE_FILES_OR_DIRECTORIES` action:

```json
{
    "lRemovedFilesOrDirectories": [
        "/DCIM/20251016/event.mp4"
    ],
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

For other actions:

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

## Capture API

The Capture API provides manual control over media capture, independent of the rule-based recording system.

### Capture Status

Get the current capture status.

**Path:** `.../vigil/capture/status`

**Method:** `GET`

**Response Body:**

```json
{
    "dLastCapture": { // Object (optional), current active capture if in progress, otherwise last completed capture, null if none
        "sID": "194e8b2c0000194e8b2c0001", // String, unique capture event ID
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, output directory path
        "sFormat": "MP4", // String, capture format ("MP4", "JPG", "RAW")
        "iVideoLengthSeconds": 60, // Integer, optional, present only for video captures with finite length
        "sStatus": "WRITING", // String, write event status ("PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN")
        "iTimestamp": 1736870400000, // Integer, Unix timestamp in milliseconds
        "sFileName": "Capture 20260114 143000 123456.mp4" // String, generated file name
    },
    "bReadyToStartNew": true, // Boolean, whether a new capture can be started (false if active capture in progress)
    "bStopRequested": false // Boolean, whether a stop capture request is pending
}
```

### Start Capture

Start a new capture session.

**Path:** `.../vigil/capture/start`

**Method:** `POST`

**Request Payload:**

```json
{
    "sOutput": "/mnt/sdcard/DCIM/20260114", // String, absolute path under storage mount point (directory or full file path)
    "sFormat": "MP4", // String, capture format: "MP4" (video), "JPG" (image), "RAW" (image)
    "iVideoLengthSeconds": 60 // Integer, optional, video clip length in seconds; omit for continuous recording
}
```

**Response Body:**

```json
{
    "dCapture": { // Object, the started capture event
        "sID": "194e8b2c0000194e8b2c0001", // String, unique capture event ID
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, output directory path
        "sFormat": "MP4", // String, capture format
        "iVideoLengthSeconds": 60, // Integer, optional, present only for video captures with finite length
        "sStatus": "PENDING", // String, initial write event status
        "iTimestamp": 1736870400000, // Integer, Unix timestamp in milliseconds
        "sFileName": "Capture 20260114 143000 123456.mp4" // String, generated file name
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

Notes:

- Returns error `EBUSY`if a capture is already in progress.
- The output path must be an absolute path under the configured storage mount point.

### Stop Capture

Stop the current capture session.

**Path:** `.../vigil/capture/stop`

**Method:** `POST`

**Request Payload:** None required (empty object `{}` accepted).

**Response Body:**

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

Notes:

- Returns error `ENOENT` if no capture is in progress.
- Returns error `EALREADY` if capture stop is already in progress.

## Files API

### Files Fetch

Fetch the file content via the relay mechanism.

**Path:** `.../vigil/relay/<relay_uuid>/<file_path>`

**Method:** `GET`

**Description:** To access files, a client must first request a `RELAY` action via the Storage Action API. This will create a temporary relay directory (symlink) identified by a UUID, returned in the `dRelayStatus` of the Storage Control API response. The client can then access files under this directory.

**Example:** `.../vigil/relay/550e8400-e29b-41d4-a716-446655440000/DCIM/20251016/event.mp4`

Notes:

- The relay directory is temporary and will timeout after a period of inactivity (default 300s).
- Accessing the relay directory refreshes the timeout.
- Use the `UNRELAY` action to explicitly close the relay.
- The default thumbnail is generated on-the-fly for video files when accessed via the relay: `/path/to/video.mp4` -> `/path/to/.thumb/video.mp4.thumb.jpg`

## Error Handling

All endpoints return a `code` field (0 on success). When `code` is non-zero, the value is error_base(30000) add the abs(posix_errno) and a `message` string may be included.

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure`
}
```
