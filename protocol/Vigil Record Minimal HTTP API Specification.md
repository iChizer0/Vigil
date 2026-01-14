# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-01-14
Version: 1.4.0

## Rule API

### General Rule Configuration

Get or set the global rule enablement status.

**Path:** `.../vigil/rule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "bRuleEnabled": true, // Boolean, enable or disable all rules
    "dWriterConfig": {
        "sFormat": "MP4", // String, supported: "MP4", "JPG", "RAW" (case-sensitive)
        "iIntervalMs": 0 // Integer, image capture interval in ms; 0 keeps continuous video
    },
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

### Rule Info

Get the information about the rule system.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "dLastRuleEvent": { // Optional, present only if a last event exists
        "sStatus": "COMPLETED", // String, write event status, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN"
        "iTimestamp": 1735574400 // Integer, Unix timestamp
    },
    "dCurrentRuleEvent": { // Optional, present only if a current event owner exists
        "sRuleType": "TIMER", // String, rule type that owns the event
        "sRuleID": "timer_rule_1", // String, rule ID
        "iTimestamp": 1735574400 // Integer, Unix timestamp
    },
    "dCurrentRuleEventOwner": { // Optional, present only if a current event owner exists
        "sRuleType": "TIMER", // String, rule type that owns the event
        "sRuleID": "timer_rule_1", // String, rule ID
        "iTimestamp": 1735574400 // Integer, Unix timestamp
    },
    "dAvailableGPIOs": {
        "GPIO_01": {
            "iNum": 1, // Integer, GPIO number
            "sState": "FLOATING", // String|null, "DISABLED", "FLOATING", "PULL_UP", "PULL_DOWN"
            "lCapabilities": ["FLOATING", "PULL_UP", "PULL_DOWN"], // Array of available input modes
            "sLevel": "HIGH" // String|null, current level if callback provided ("HIGH" or "LOW")
        }
    },
    "dAvailableTTYs": {
        "ttyS0": {
            "sSocketPath": "/tmp/ttyS0.sock", // String, Unix socket path
            "iBufferSize": 1024 // Integer, buffer size in bytes
        }
    },
    "bMediaPaused": false // Boolean, whether media capture is currently paused
}
```

### Schedule Rule Configuration

Get or set the schedule rule configuration.

**Path:** `.../vigil/rule/schedule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "bEnabled": true, // Boolean, enable or disable schedule rule
    "lActiveWeekdays": [
        {
            "sStart": "Mon 09:00:00", // String, format: "Day HH:MM:SS" (Day: Sun, Mon, Tue, Wed, Thu, Fri, Sat; Hour 24:00:00 is valid for end-of-day)
            "sEnd": "Mon 18:00:00" // String, weekday name (short) + ISO 8601 time
        }
        // ... More time ranges, e.g., full week: {"sStart": "Mon 00:00:00", "sEnd": "Sun 24:00:00"}
    ],
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
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
    "sCurrentSelected": "INFERENCE_SET",
    "lInferenceSet": [
        {
            "sID": "person_detection", // String, unique rule ID
            "iDebounceTimes": 3, // Integer, number of consecutive frames to confirm detection
            "lConfidenceFilter": [0.5, 1.0], // Array [min, max], confidence range
            "lClassFilter": [0, 1], // Array of integers, class IDs to filter
            "lRegionFilter": [ // Optional, list of polygons
                {
                    "lPolygon": [
                        [0.0, 0.0], [0.5, 0.0], [0.5, 0.5], [0.0, 0.5] // List of [x, y] points (0.0-1.0)
                    ]
                }
            ]
        }
    ],
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

#### Timer Rule

```json
{
    "sCurrentSelected": "TIMER",
    "dTimer": {
        "iIntervalSeconds": 60 // Integer, trigger interval in seconds
    },
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

#### GPIO Rule

```json
{
    "sCurrentSelected": "GPIO",
    "dGPIO": {
        "sName": "GPIO_01", // String, GPIO pin name (or provide "iNum" instead)
        "sState": "FLOATING", // String, "DISABLED", "FLOATING", "PULL_UP", "PULL_DOWN"
        "sSignal": "HIGH", // String, "HIGH", "LOW", "RISING", "FALLING"
        "iDebounceDurationMs": 100 // Integer, must be >= 0
    },
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

#### TTY Rule

```json
{
    "sCurrentSelected": "TTY",
    "dTTY": {
        "sName": "ttyS0", // String, TTY name as defined in system config
        "sCommand": "RECORD" // String, non-empty command string to trigger recording
    },
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

#### HTTP Rule

The HTTP rule requires no additional configuration; select it to enable external trigger via the activation API.

```json
{
    "sCurrentSelected": "HTTP",
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

### HTTP Rule Activation

Trigger the currently selected HTTP rule (no payload required).

**Path:** `.../vigil/rule/http_rule/activate`

**Method:** `POST`

**Response Body:**

```json
{
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

## Storage API

### Storage Configuration

Get or set the storage configuration.

**Path:** `.../vigil/storage/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "sTargetEnabledSlotDevPath": "/dev/sda1", // String, device path to enable; empty string disables all
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
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
            "sDevPath": "/dev/sda1", // String
            "sMountPath": "/mnt/sda1", // String
            "bRemovable": true, // Boolean
            "bInternal": false, // Boolean
            "sLabel": "MyDrive", // String|null
            "sUUID": "550e8400-e29b-41d4-a716-446655440000", // String|null
            "sType": "ext4", // String|null
            "bEnabled": true, // Boolean
            "bWriting": false, // Boolean
            "eState": 8, // Integer, SlotState enum value (1: ERROR, 2: NOT_FORMATTED_OR_FORMAT_UNSUPPORTED, 3: FORMATTING, 4: NOT_MOUNTED, 5: MOUNTED, 6: CONFIGURED, 7: INDEXING, 8: READY)
            "sState": "READY", // String, human-readable state ("ERROR", "NOT_FORMATTED_OR_FORMAT_UNSUPPORTED", "FORMATTING", "NOT_MOUNTED", "MOUNTED", "CONFIGURED", "INDEXING", "READY")
            "iStatsSizeBytes": 1000000000, // Integer, total size
            "iStatsFreeBytes": 500000000, // Integer, free space
            "iQuotaLimitBytes": 100000000, // Integer
            "iQuotaUsedBytes": 10000000, // Integer|null if unknown
            "bQuotaRotate": true // Boolean
        }
    ],
    "sDataDirName": "DCIM", // String
    "sCurrentEnabledSlotDevPath": "/dev/sda1", // String, currently enabled slot
    "sTargetEnabledSlotDevPath": "/dev/sda1", // String, target slot from config
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

### Storage Control

Perform an action on the storage system.

**Path:** `.../vigil/storage/control`

**Method:** `POST`

**Request Payload:**

```json
{
    "sAction": "FREE_UP", // String, one of: "FORMAT", "FREE_UP", "EJECT", "CONFIG", "RELAY", "RELAY_STATUS", "UNRELAY", "REMOVE_FILES_OR_DIRECTORIES"
    "sSlotDevPath": "/dev/sda1", // String, device path of the slot
    "dSlotConfig": { // Required only for "Config" action
        "iQuotaLimitBytes": 1073741824, // Integer
        "bQuotaRotate": true // Boolean
    },
    "lFilesOrDirectoriesToRemove": [ // Required only for "RemoveFilesOrDirectories" action; use paths relative to the data directory
        "DCIM/20251016/event.mp4"
    ]
}
```

**Response Body:**

The response depends on the `sAction`.

For `Relay` and `RelayStatus`:
```json
{
    "dRelayStatus": {
        "iRelayTimeoutRemain": 25,
        "iRelayTimeout": 300,
        "sRelayDirectory": "550e8400-e29b-41d4-a716-446655440000"
    },
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

For `RemoveFilesOrDirectories`:
```json
{
    "lRemovedFilesOrDirectories": [
        "/DCIM/20251016/event.mp4"
    ],
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

For other actions:
```json
{
    "code": 0 // Integer, POST only, 0 on success, errors return 30000 + |errno|
}
```

## Capture API

The Capture API provides manual control over media capture, independent of the rule-based recording system.

### Capture Status

Get the current capture status including any ongoing and last completed capture.

**Path:** `.../vigil/capture/status`

**Method:** `GET`

**Response Body:**

```json
{
    "dCurrent": { // Object|null, present only if a capture is in progress
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, output directory path
        "sFormat": "MP4", // String, capture format ("MP4", "JPG", "RAW")
        "iVideoClipLengthSeconds": 60, // Integer, optional, present only for video captures with finite length
        "sStatus": "WRITING", // String, write event status ("PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN")
        "sTimepoint": 1736870400000000000, // String, steady clock timepoint (nanoseconds since epoch)
        "iTimestamp": 1736870400000, // Integer, Unix timestamp in milliseconds
        "sFileName": "CAP 20260114 143000 123456.mp4" // String, generated file name
    },
    "dLast": { // Object|null, present only if a previous capture exists
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, output directory path
        "sFormat": "JPG", // String, capture format
        "sStatus": "COMPLETED", // String, write event status
        "sTimepoint": 1736866800000000000, // String, steady clock timepoint
        "iTimestamp": 1736866800000, // Integer, Unix timestamp in milliseconds
        "sFileName": "CAP 20260114 133000 654321.jpg" // String, generated file name
    }
}
```

### Start Capture

Start a new capture session.

**Path:** `.../vigil/capture/start`

**Method:** `POST`

**Request Payload:**

```json
{
    "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, absolute path under storage mount point
    "sFormat": "MP4", // String, capture format: "MP4" (video), "JPG" (image), "RAW" (image)
    "iVideoLengthSeconds": 60 // Integer, optional, video clip length in seconds; omit for continuous recording
}
```

**Response Body:**

```json
{
    "code": 0 // Integer, 0 on success, errors return 30000 + |errno|
}
```

**Notes:**
- Returns error `EBUSY` (30016) if a capture is already in progress.
- The output directory must be an absolute path under the configured storage mount point.

### Stop Capture

Stop the current capture session.

**Path:** `.../vigil/capture/stop`

**Method:** `POST`

**Request Payload:** None required (empty object `{}` accepted).

**Response Body:**

```json
{
    "code": 0 // Integer, 0 on success, errors return 30000 + |errno|
}
```

**Notes:**
- Returns error `ENOENT` (30002) if no capture is in progress.
- Returns error `EALREADY` (30114) if capture stop is already in progress.

## Files API

### Files Fetch

Fetch the file content via the relay mechanism.

**Path:** `.../vigil/relay/<relay_uuid>/<file_path>`

**Method:** `GET`

**Description:**
To access files, a client must first request a `relay` action via the Storage Action API. This will create a temporary relay directory (symlink) identified by a UUID, returned in the `dRelayStatus` of the Storage Control API response. The client can then access files under this directory.

**Example:**
`.../vigil/relay/550e8400-e29b-41d4-a716-446655440000/DCIM/20251016/event.mp4`

**Notes:**
- The relay directory is temporary and will timeout after a period of inactivity (default 300s).
- Accessing the relay directory refreshes the timeout.
- Use the `Unrelay` action to explicitly close the relay.

## Error Handling

All endpoints return a `code` field (0 on success). When `code` is non-zero, the value is `30000 + |errno|` and a `message` string may be included.

```json
{
    "code": 30022, // Integer, POST only, 0 on success, errors return 30000 + |errno|
    "message": "Invalid argument" // String, error description (present only on failure)
}
```
