# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-04-17
Version: 1.6.0

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
    "bReadyForNewEvent": true, // Boolean, whether a new recording event can be started (false if a current event is still writing)
    "dLastRuleEvent": { // Optional, present only if a finished event exists, updated on current event finished
        "sStatus": "COMPLETED", // String, write event status, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN"
        "iTimestamp": 1735574000 // Integer, Unix timestamp in milliseconds
    },
    "dLastRuleEventOwner": { // Optional, present and update when a rule evaluated as true and the current event is finished
        "sRuleType": "TIMER", // String, rule type
        "sRuleID": "Hourly Timer", // String, rule ID
        "iTimestamp": 1735574400 // Integer, Unix timestamp in milliseconds (should be same as the corresponding timestamp of its event)
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
    "sCurrentSelected": "INFERENCE_SET", // String, one of: "INFERENCE_SET", "TIMER", "GPIO", "TTY", "HTTP", "ALWAYS_ON"
    "lInferenceSet": [
        {
            "sID": "person_detection", // String, rule ID (uniqueness not enforced but recommended)
            "iDebounceTimes": 3, // Integer, number of consecutive frames to confirm the activation
            "lConfidenceFilter": [0.5, 1.0], // Array of number [min, max], confidence range to filter, configurable range between 0.0 and 1.0 for each part
            "lClassFilter": ["person", "car"], // Array of strings, class names to filter
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

#### Always-On Rule

The always-on rule requires no additional configuration. When selected, it continuously re-arms using the current
`dWriterConfig.iIntervalMs` pacing. If `iIntervalMs` is `0`, the rule becomes eligible again immediately after the
previous write finishes.

```json
{
    "sCurrentSelected": "ALWAYS_ON",
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
    "dSelectSlotToEnable": { // Object, set to null to disable enabled slot or not enable any slot
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
            "bSyncing": false, // Boolean, whether this slot is currently performing sync tasks (e.g., flushing write cache)
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

The request uses two routing fields:

- `sAction`: the storage operation family
- `sTaskType`: how that operation should be handled

Supported `sAction` values:

- `FORMAT`
- `FREE_UP`
- `EJECT`
- `REMOVE_FILES_OR_DIRECTORIES`
- `CONFIG`
- `RELAY`
- `RELAY_STATUS`
- `UNRELAY`

Supported `sTaskType` values:

- `SYNC`
- `ASYNC_SUBMIT`
- `ASYNC_STATUS`
- `ASYNC_CANCEL`

If `sTaskType` is omitted, it defaults to `SYNC`.

Async task types are intended for the long-running action families:

- `FORMAT`
- `FREE_UP`
- `EJECT`
- `REMOVE_FILES_OR_DIRECTORIES`

The following actions are synchronous only:

- `CONFIG`
- `RELAY`
- `RELAY_STATUS`
- `UNRELAY`

**Request Payload:**

```json
{
    "sTaskType": "ASYNC_STATUS", // String (optional), one of: "SYNC", "ASYNC_SUBMIT", "ASYNC_STATUS", "ASYNC_CANCEL", defaults to "SYNC"
    "sAction": "FREE_UP", // String, one of: "FORMAT", "FREE_UP", "EJECT", "REMOVE_FILES_OR_DIRECTORIES", "CONFIG", "RELAY", "RELAY_STATUS", "UNRELAY"
    "sSlotDevPath": "/dev/sda1", // String, required for all actions, device path of the slot
    "dSlotConfig": { // Object, required only for "CONFIG" action
        "iQuotaLimitBytes": 1073741824, // Integer, quota limit in bytes, -1 means no limit
        "bQuotaRotate": true // Boolean, whether to enable quota rotation
    },
    "lFilesOrDirectoriesToRemove": [ // List, required only for "SYNC" or "ASYNC_SUBMIT" with "REMOVE_FILES_OR_DIRECTORIES"
        "DCIM/20251016/event.mp4", // String, relative path under the record data directory, the sanitized path must still be under the record data directory, otherwise error may returned
        // ... More files or directories to remove
    ]
}
```

**Response Body:**

Top-level response fields keep their existing meaning:

- `code == 0`: the request itself was valid and accepted.
- `message`: request-level failure only.

For `SYNC` requests:

- `CONFIG`, `RELAY`, `RELAY_STATUS`, and `UNRELAY` behave synchronously as before
- `FORMAT`, `FREE_UP`, `EJECT`, and `REMOVE_FILES_OR_DIRECTORIES` perform the operation synchronously and return only after the operation finishes

For `ASYNC_SUBMIT`, `ASYNC_STATUS`, and `ASYNC_CANCEL` requests, the background-task history for the requested `{sAction, sSlotDevPath}` is returned in `dTasks`.

### Async Task History

```json
{
    "dTasks": {
        "1776353445000": { // Key: String, task creation timestamp in milliseconds
            "iCreatedTimestamp": 1776353445000, // Integer, task creation timestamp in milliseconds
            "sAction": "FREE_UP", // String, async action family: "FORMAT", "FREE_UP", "EJECT", "REMOVE_FILES_OR_DIRECTORIES"
            "sSlotDevPath": "/dev/sda1", // String, target slot device path
            "sStatus": "RUNNING", // String, one of: "QUEUED", "RUNNING", "SUCCEEDED", "FAILED", "CANCEL_REQUESTED", "CANCELED"
            "bDone": false, // Boolean, true when the task reached a final state
            "bCancelable": true, // Boolean, whether cancel may still succeed now
            "iQueuePosition": 0, // Integer (optional), 0 for running, 1 for next queued task, null when done
            "iProgressPercent": 42, // Integer (optional), 0-100 if known, otherwise null
            "iUpdatedTimestamp": 1776353448000, // Integer, Unix timestamp in milliseconds
            "dResult": { // Object, task result payload and final result metadata
                "iCode": null, // Integer (optional), final POSIX-style result code when done
                "sMessage": null, // String (optional), final human-readable result message
                "iTotalEntries": 10,
                "iProcessedCount": 4,
                "iRemovedCount": 4,
                "iFailedCount": 0
            }
        }
    },
    "code": 0,
    "message": ""
}
```

The `dResult` payload shape depends on the async action family:

- `FORMAT` and `EJECT`: `iCode`, `sMessage`
- `FREE_UP`: `iCode`, `sMessage`, `iTotalEntries`, `iProcessedCount`, `iRemovedCount`, `iFailedCount`
- `REMOVE_FILES_OR_DIRECTORIES`: `iCode`, `sMessage`, `iRequestedCount`, `iRemovedCount`, `iFailedCount`, and final `lRemovedFilesOrDirectories`

For async submit, status, and cancel requests:

- top-level `code` reports request acceptance/rejection
- `dTasks` is an object keyed by task creation timestamps
- each `dTasks.<created_timestamp>.dResult.iCode` reports the final task result when that task is done
- each `dTasks.<created_timestamp>.dResult.sMessage` provides the final task result message when that task is done
- at most the three most recent tasks are retained for the requested `{sAction, sSlotDevPath}`

The task identity is:

- `sAction`
- `sSlotDevPath`

### Async Action Semantics

- `FORMAT`
    - `ASYNC_SUBMIT` queues a formatting task
    - `ASYNC_STATUS` returns up to the three most recent format tasks for the slot
    - `ASYNC_CANCEL` cancels unfinished queued format tasks for the slot
  - cancelable while queued
  - usually not cancelable after formatting starts
- `FREE_UP`
    - `ASYNC_SUBMIT` queues a free-up task
    - `ASYNC_STATUS` returns up to the three most recent free-up tasks for the slot
    - `ASYNC_CANCEL` cancels unfinished queued free-up tasks and requests cooperative cancellation for a running task
  - cancelable while queued
  - cooperatively cancelable while running
- `EJECT`
    - `ASYNC_SUBMIT` queues an eject task
    - `ASYNC_STATUS` returns up to the three most recent eject tasks for the slot
    - `ASYNC_CANCEL` cancels unfinished queued eject tasks for the slot
  - cancelable while queued
  - usually not cancelable after eject starts
- `REMOVE_FILES_OR_DIRECTORIES`
    - `ASYNC_SUBMIT` queues a file-removal task
    - `ASYNC_STATUS` returns up to the three most recent file-removal tasks for the slot
    - `ASYNC_CANCEL` cancels unfinished queued file-removal tasks and requests cooperative cancellation for a running task
  - cancelable while queued
  - cooperatively cancelable while running

For synchronous-only actions (`CONFIG`, `RELAY`, `RELAY_STATUS`, `UNRELAY`), using `ASYNC_SUBMIT`, `ASYNC_STATUS`, or `ASYNC_CANCEL` is invalid.

For `REMOVE_FILES_OR_DIRECTORIES`, each final history entry stores aggregate counts and removed-path details in `dTasks.<created_timestamp>.dResult`:

```json
{
    "dTasks": {
        "1776353470000": {
            "iCreatedTimestamp": 1776353470000,
            "sAction": "REMOVE_FILES_OR_DIRECTORIES",
            "sSlotDevPath": "/dev/sda1",
            "sStatus": "SUCCEEDED",
            "bDone": true,
            "bCancelable": false,
            "iQueuePosition": null,
            "iProgressPercent": 100,
            "iUpdatedTimestamp": 1776353472000,
            "dResult": {
                "iCode": 0,
                "sMessage": "Completed",
                "iRequestedCount": 3,
                "iRemovedCount": 2,
                "iFailedCount": 1,
                "lRemovedFilesOrDirectories": [
                    "DCIM/20251016/event.mp4",
                    "DCIM/20251016/thumbnail.jpg"
                ]
            }
        }
    },
    "code": 0
}
```

If no retained task history exists for an `ASYNC_STATUS` or `ASYNC_CANCEL` request, the API returns a request-level failure such as `ENOENT`.

If a cancel request arrives too late, the API returns a request-level failure and may still include the current `dTasks` history.

Queueing rules are intentionally simple:

- only one mutating task runs at a time
- up to three unfinished async tasks may exist for the same `sSlotDevPath`
- if the per-slot unfinished-task limit is reached, submit fails
- `dTasks` retains up to three recent tasks for the requested `{sAction, sSlotDevPath}`

For `SYNC` `RELAY` and `RELAY_STATUS` requests:

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

For `SYNC` `CONFIG` and `UNRELAY` requests:

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

For `SYNC` `REMOVE_FILES_OR_DIRECTORIES` requests:

```json
{
    "lRemovedFilesOrDirectories": [
        "DCIM/20251016/event.mp4"
    ],
    "code": 0,
    "message": ""
}
```

Notes:

- This synchronous response lists only successfully removed paths.
- Aggregate per-path failure counts are tracked only in async task history results; failed-path detail is not exposed.

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
