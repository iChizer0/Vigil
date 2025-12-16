# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2025-12-11
Version: 1.2.0

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
        "sFormat": "mp4", // String, "mp4", "jpg", "raw"
        "iIntervalMs": 0 // Integer, interval for image capture, 0 for video
    },
    "code": 0 // Integer, 0 on success, >0 on error
}
```

### Schedule Rule Configuration

Get or set the schedule rule configuration.

**Path:** `.../vigil/rule/schedule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "lActiveWeekdays": [
        {
            "dStart": {
                "iDay": 1, // Integer, 0-6 (Sunday-Saturday)
                "iHour": 9, // Integer, 0-23
                "iMinute": 0, // Integer, 0-59
                "iSecond": 0 // Integer, 0-59
            },
            "dEnd": {
                "iDay": 1,
                "iHour": 18,
                "iMinute": 0,
                "iSecond": 0
            }
        }
        // ... More time ranges
    ],
    "code": 0
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
    "sCurrentSelected": "InferenceSet",
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
    "code": 0
}
```

#### Timer Rule

```json
{
    "sCurrentSelected": "Timer",
    "dTimer": {
        "iIntervalSeconds": 60 // Integer, trigger interval in seconds
    },
    "code": 0
}
```

#### GPIO Rule

```json
{
    "sCurrentSelected": "GPIO",
    "dGPIO": {
        "sName": "GPIO_01", // String, GPIO pin name as defined in system config
        "sInitialLevel": "low", // String, "high", "low"
        "sSignal": "high", // String, "high", "low", "rising", "falling"
        "iDebounceDurationMs": 100 // Integer, debounce duration in milliseconds
    },
    "code": 0
}
```

#### TTY Rule

```json
{
    "sCurrentSelected": "TTY",
    "dTTY": {
        "sName": "ttyS0", // String, TTY name as defined in system config
        "sCommand": "RECORD" // String, command string to trigger recording
    },
    "code": 0
}
```

### Rule Info

Get the available hardware resources for rules.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "dAvailableGPIOs": {
        "GPIO_01": {
            "iNum": 1, // Integer, GPIO number
            "bActiveHigh": true // Boolean, active high or low
        }
    },
    "dAvailableTTYs": {
        "ttyS0": {
            "sFIFOPath": "/tmp/ttyS0", // String, path to FIFO
            "iBufferSize": 1024 // Integer, buffer size in bytes
        }
    },
    "code": 0
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
    "sTargetEnabledSlotDevPath": "/dev/sda1", // String, device path to enable; empty to disable all
    "code": 0
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
            "sLabel": "MyDrive", // String
            "sUUID": "550e8400-e29b-41d4-a716-446655440000", // String
            "sType": "ext4", // String
            "bEnabled": true, // Boolean
            "bWriting": false, // Boolean
            "eState": 1, // Integer, SlotState enum value (1: Error, 2: NotFormattedOrFormatUnsupported, 3: Formatting, 4: NotMounted, 5: Mounted, 6: Configured, 7: Indexing, 8: Ready)
            "sState": "Error", // String, human-readable state
            "iStatsSizeBytes": 1000000000, // Integer, total size
            "iStatsFreeBytes": 500000000, // Integer, free space
            "iQuotaLimitBytes": 100000000, // Integer
            "iQuotaUsedBytes": 10000000, // Integer
            "bQuotaRotate": true // Boolean
        }
    ],
    "sDataDirName": "DCIM", // String
    "sCurrentEnabledSlotDevPath": "/dev/sda1", // String, currently enabled slot
    "sTargetEnabledSlotDevPath": "/dev/sda1", // String, target slot from config
    "code": 0
}
```

### Storage Control

Perform an action on the storage system.

**Path:** `.../vigil/storage/control`

**Method:** `POST`

**Request Payload:**

```json
{
    "sAction": "free_up", // String, "format", "free_up", "eject", "config", "relay", "relay_status", "unrelay", "remove_files_or_directories"
    "sSlotDevPath": "/dev/sda1", // String, device path of the slot
    "dSlotConfig": { // Optional, required only for "config" action
        "iQuotaLimitBytes": 1073741824, // Integer
        "bQuotaRotate": true // Boolean
    },
    "lFilesOrDirectoriesToRemove": [ // Optional, required only for "remove_files_or_directories" action
        "/DCIM/20251016/event.mp4"
    ]
}
```

**Response Body:**

The response depends on the `sAction`.

For `relay` and `relay_status`:
```json
{
    "dRelayStatus": {
        "iRelayTimeoutRemain": 25,
        "iRelayTimeout": 30,
        "sRelayDirectory": "550e8400-e29b-41d4-a716-446655440000"
    },
    "code": 0
}
```

For `remove_files_or_directories`:
```json
{
    "lRemovedFilesOrDirectories": [
        "/DCIM/20251016/event.mp4"
    ],
    "code": 0
}
```

For other actions:
```json
{
    "code": 0
}
```

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
- The relay directory is temporary and will timeout after a period of inactivity (default 30s).
- Accessing the relay directory refreshes the timeout.
- Use the `unrelay` action to explicitly close the relay.

## Error Handling

All endpoints return a `code` field (0 on success). When `code` is non-zero, the value is `|errno| + offset` and a `message` string may be included.

```json
{
    "code": 22, // Integer, 0 on success; error values are |errno| + offset
    "message": "Invalid argument" // String, error description (present only on failure)
}
```
