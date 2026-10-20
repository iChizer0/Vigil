# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-10-19
Version: 2.0.0

## Rule API

### General Rule Configuration

Get or set the rule enablement and the post-trigger actions. The payload is the version-2 record configuration
(`record_config.json`): the library accepts no other version and never rewrites a stored file it cannot read, so an
external manager migrates older files. `GET` returns the canonical document; `POST` takes the document shown below
and stores it in canonical form; every field is required (see below).

**Path:** `.../vigil/rule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "iSchemaVersion": 2, // Integer, required, must be 2
    "bRuleEnabled": true, // Boolean, required, enable or disable the evaluation of all rules, evaluate precedence: schedule rule > record rule
    "bContinuousRecordEnabled": false, // Boolean, required, standalone continuous MP4 recording; not supported yet, must be false
    "dActions": { // Object, required, the post-trigger actions
        "dPhoto": {
            "bEnabled": false, // Boolean, required
            "sFormat": "JPG", // String, required, case-sensitive, one of: "JPG", "RAW"
            "iIntervalMs": 5000 // Integer, required, positive, the delay between captures in milliseconds including the write duration
        },
        "dVideo": {
            "bEnabled": true, // Boolean, required; the format is always MP4
            "iIntervalMs": 5000 // Integer, required, positive, the delay between recordings in milliseconds including the write duration
        },
        "dSound": {
            "bEnabled": false // Boolean, required, play a sound on trigger through the host's playback callback; without one the action is unavailable and a trigger plays nothing
        }
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

The document is always complete: `GET` returns every field, and `POST` takes the whole document back (edit the `GET` response
and post it). Any combination of actions may be enabled, and `bRuleEnabled: true` with every action disabled is accepted:
every rule event is reported and nothing else happens. Photo and Video run independently, each on its own interval, and
may both publish a file for one rule event; Sound plays through the host's callback and is never queued (a trigger while
one is pending or playing is dropped). `bContinuousRecordEnabled` is rejected when true until continuous recording lands.
With `ALWAYS_ON` selected (the default), the actions are limited to what it paces (see Always-On Rule): a payload outside
that limit is rejected with `EINVAL`; select another record rule first. A missing field, a field of the wrong type, a
non-positive or non-integer interval, an unknown photo format, and any other `iSchemaVersion` are rejected with `EINVAL`
(`30022`); a rejected payload changes neither the runtime nor the stored file. A SED rule with Sound enabled can
retrigger itself through the microphone; any guard time belongs to the host's playback. Unknown fields are
ignored and dropped from the stored document. `EAGAIN` (`30011`) means the action settings changed (a media refresh or
another commit) while the document was being prepared or applied; post it again. Request bodies are limited to 1 MiB; a
larger body is rejected before parsing (the C API returns `EFAULT` with no JSON body). A first start without
`record_config.json` reports every action disabled.

### Rule Info

Get the information about the rule system.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "bReadyForNewEvent": true, // Boolean, whether a new recording event can be started (false while a file channel holds a task: pending, writing, or finished but still within its interval and failure cooldown)
    "dLastRuleEvent": { // Object or null: the most recently accepted file task: the held one while a file channel holds a task (the one accepted last when both do), otherwise the last retired one
        "sStatus": "COMPLETED", // String, write event status, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED" (a gate, pause, stop, or settings change stopped a task that had started; its file is kept), "CANCELED" (nothing was recorded: retired before a writer claimed it, or the gate was already closed when the writer started), "UNKNOWN"
        "iTimestamp": 1735574000, // Integer, Unix timestamp in milliseconds
        "iOwnerUID": 1735574400123 // Integer, UID of the owner that created the event (exact 64-bit value)
    },
    "dLastRuleEventOwner": { // Object or null: the owner of dLastRuleEvent
        "sRuleType": "TIMER", // String, rule type; "UNKNOWN" for a direct EVENT delivered without a rule
        "sRuleID": "Hourly Timer", // String, the condition: the inference member ID, the GPIO signal (e.g. "RISING"), or the TTY command; empty for other rules
        "sSourceID": "", // String, the inference source for inference and direct EVENT owners, the pin name for GPIO, the TTY name for TTY, otherwise empty
        "sSourceDelivery": "EVENT", // String, "FRAME" for inference frame owners, "EVENT" for every other owner
        "iTimestamp": 1735574400, // Integer, event time in Unix milliseconds: the input's timestamp for FRAME and EVENT owners, otherwise the time the condition was observed
        "iDispatchTimestamp": 1735574400, // Integer, Unix milliseconds when Vigil created the owner; equals iTimestamp for owners without an input
        "iUID": 1735574400123 // Integer, unique owner ID (exact 64-bit value), also carried by the rule and file callbacks
    },
    "dLastDispatchedOwner": { // Object or null: the owner of the most recent dispatch, whatever the actions did with it, in the shape of dLastRuleEventOwner; it moves ahead of dLastRuleEventOwner when the file actions rejected or did not apply to the event
        // ...
    },
    "dActionStates": { // Object, the state of each post-trigger action
        "dPhoto": {
            "bEnabled": true, // Boolean, whether the action is enabled in the general rule configuration
            "bReady": false, // Boolean, enabled and holding no task: the next event can be admitted here, subject to the gates and the storage slot
            "sState": "WRITING", // String, one of: "IDLE" (no task held), "PENDING" (accepted, no worker has claimed it yet), "WRITING", "HOLDING" (the task finished, sStatus is terminal, and the channel still holds it: through its interval after a completed task, or its failure cooldown after a failed one; an interrupted or canceled task is retired at once)
            "dEvent": { // Object or null: the held task's event, in the shape of dLastRuleEvent
                "sStatus": "WRITING",
                "iTimestamp": 1735574000,
                "iOwnerUID": 1735574400123
            }
        },
        "dVideo": {
            // ... The same fields as dPhoto
        },
        "dSound": {
            "bEnabled": false, // Boolean, whether the action is enabled in the general rule configuration
            "bReady": false, // Boolean, enabled, available, and idle: the next event can trigger playback, subject to the gates
            "sState": "UNAVAILABLE" // String, one of: "UNAVAILABLE" (the host provides no playback callback), "IDLE", "PENDING" (triggered, not yet playing), "PLAYING", "STOPPING" (the service is shutting down)
        }
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
    "bVideoClipLengthSeconds": 60, // Integer, target video clip length in seconds (estimated, may inaccurate because the GOP length varies)
    "bMediaPaused": false, // Boolean, whether media capture is currently paused (e.g., due to configuration update in underlying encoder system)
    "bRecordEnabled": true, // Boolean, whether the record rule is currently enabled, the schedule rule is also required to be enabled for recording to be triggered
    "dRecordSlot": {
        // ... Refer to the storage slot object in Storage Status API response for the schema of this field, null if no available slot is ready for recording
    }
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
    "sCurrentSelected": "INFERENCE_SET", // String, one of: "SED", "INFERENCE_SET", "TIMER", "GPIO", "TTY", "HTTP", "ALWAYS_ON"
    "lInferenceSet": [
        {
            "sID": "person_detection", // String, rule ID (uniqueness not enforced but recommended)
            "lSourceFilter": ["builtin", "fall-detection"], // Optional; array of source ids to consume, empty or absent matches every source. "builtin" is the on-device inference pipeline
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

`lSourceFilter` selects the result sources this rule consumes, matched against the canonical source id. Source ids are stamped by the receiving ingress from the peer identity; a producer's self-reported `source_id` is never honored verbatim. An absent or empty filter matches every source, preserving the pre-source-filter contract under which rules consumed builtin and validated ResultSink traffic. Results from unlisted sources neither advance nor decay this rule's debounce state.

Applications whose post-processing does not fit the detection/classification result formats decide on recording internally and emit a discrete event (`INFERENCE_DELIVERY_EVENT` with no result payload) instead. Subject to the schedule rule, such an event triggers recording immediately with the source id as the event id, bypassing rule filters and debounce entirely.

#### Sound Event Rule

```json
{
    "sCurrentSelected": "SED",
    "dSED": {
        "sID": "00000000-0000-0000-0000-000000000000", // String, AcousticsLab head/model ID to match against inference head_id; empty string accepts any model
        "iConsecutiveWindowMs": 1000, // Integer, required consecutive active duration in milliseconds before activation, must be >= 0
        "lConfidenceFilter": [0.5, 1.0], // Array of number [min, max], confidence range to filter, configurable range between 0.0 and 1.0
        "lClassFilter": ["doorbell", "baby_cry"] // Array of strings, class labels to filter; empty array accepts any label
    },
    "code": 0,
    "message": ""
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

Rule events from a GPIO rule (the rule callback and `dLastRuleEventOwner`) carry the signal as the rule ID and the pin name as the source ID.

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

Rule events from a TTY rule carry the command as the rule ID and the TTY name as the source ID.

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

The always-on rule requires no additional configuration. When selected, it continuously re-arms using the enabled
file action's `iIntervalMs` pacing (see General Rule Configuration): the next recording starts no earlier than the
interval after the previous one started, and only once the previous write has finished.

Because it paces one file action, selecting `ALWAYS_ON` requires the stored general rule configuration to satisfy its
invariant: `dSound` disabled, at most one of `dPhoto` and `dVideo` enabled, and exactly one while `bRuleEnabled` is true
(the first-start configuration satisfies it). The record rule `POST` that selects it is rejected with `EINVAL` otherwise,
and while it is selected the general rule configuration `POST` is held to the same limit, so simplify the actions first
or select another rule first. The two checks share one lock, so neither ordering nor concurrent posts bypass it. At
startup a stored pair that violates it keeps rules disabled (`bRuleEnabled` reads false) with both files left in place.

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
- Returns error `EBUSY` if a file action is enabled and the storage system is not ready for writing; a rule-only or Sound-only activation needs no storage.
- Returns error `EBUSY` if a pending activation request is still being processed.
- Returns error `EBUSY` if a file action is enabled and no file channel accepted the event (every enabled channel still holds a task); the event is still reported.
- A refused activation leaves no pending event behind: the next activation is admitted afresh (since 2026-10-19).

## Storage API

### Storage Configuration

Mount-root ownership follows a shared-filesystem contract: the internal storage root (`/userdata`, `root:root 01777`) and any bind alias of it are owned by the factory image, retaining admin/application write access while protecting root-owned configuration entries; the recording service never modifies them. Vigil applies the historical `1000:1000 0777` policy only to slots the integrator declared external via the storage C API (`internal_device_path`), and recording subdirectories are unchanged.

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
            "bDirty": false, // Boolean (optional), whether the filesystem on this slot is marked dirty and may need fsck (null is not checked or no able to determine)
            "bSyncing": false, // Boolean, whether this slot is currently performing sync tasks (e.g., flushing write cache)
            "bWriting": false, // Boolean, whether this slot currently has activate writing tasks (e.g., recording videos, writing images)
            "bRotating": false, // Boolean, whether this slot is currently performing quota rotation
            "eState": 8, // Integer, states enum of current slot, values (-1: ERROR, 1: NOT_FORMATTED_OR_FORMAT_UNSUPPORTED, 2: FORMATTING, 3: NOT_MOUNTED, 4: FILESYSTEM_CHECKING, 5: MOUNTED, 6: CONFIGURED, 7: INDEXING, 8: READY, 9: SATURATED)
            "sState": "READY", // String, human-readable state ("ERROR", "NOT_FORMATTED_OR_FORMAT_UNSUPPORTED", "FORMATTING", "NOT_MOUNTED", "FILESYSTEM_CHECKING", "MOUNTED", "CONFIGURED", "INDEXING", "READY", "SATURATED")
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
    "sDataDirPath": "DCIM" // String, path of the record data directory under each storage mount point
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
- The `bDirty` flag indicates that the filesystem is marked dirty and may require a check (fsck) before it can be used again. This can happen if the storage device was removed without proper unmounting, or if there were write errors. A dirty filesystem may not be automatically mounted until it is repaired (if not enabled).
- The saturation state indicates that the available free space is critically low, and the system is stopping new recordings to prevent running out of space.

### Storage Control

Perform a sync or async storage action.

**Path:** `.../vigil/storage/control`

**Method:** `POST`

**Request Payload:**

```json
{
    "sTask": "ASYNC_STATUS", // String (optional), control operation task type, one of: "SYNC", "ASYNC_SUBMIT", "ASYNC_STATUS", "ASYNC_CANCEL", defaults to "SYNC"
    "sAction": "FREE_UP", // String, action type, one of: "FORMAT", "FREE_UP", "EJECT", "REMOVE_FILES_OR_DIRECTORIES", "CONFIG", "RELAY", "RELAY_STATUS", "UNRELAY"
    "sSlotDevPath": "/dev/sda1", // String, device path of the target slot, required for every action
    "sTaskUID": "0-12345678901234567890", // String (optional), queried by ASYNC_STATUS / ASYNC_CANCEL. Current behavior: non-empty is ignored, empty forces no matches
    "dSlotConfig": { // Object (optional), required only for sync "CONFIG" action
        "iQuotaLimitBytes": 1073741824, // Integer, quota limit in bytes, -1 for no limit
        "bQuotaRotate": true // Boolean, whether quota rotation is enabled
    },
    "lFilesOrDirectoriesToRemove": [ // Array of strings (optional), required only for "REMOVE_FILES_OR_DIRECTORIES" action with SYNC or ASYNC_SUBMIT
        "DCIM/20251016/event.mp4" // String, relative path to file or directory
    ]
}
```

Notes:

- Async `sTask` (`ASYNC_*`) is supported only by `FORMAT`, `FREE_UP`, `EJECT`, and `REMOVE_FILES_OR_DIRECTORIES`.
- `CONFIG`, `RELAY`, `RELAY_STATUS`, and `UNRELAY` are sync-only operations.

**Response Body:**

The payload depends on the `sTask` field.

#### Sync (SYNC)

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "", // String, optional, present only on failure
    "dRelayStatus": { // Object (optional), present only for "RELAY" and "RELAY_STATUS" on success
        "iRelayTimeoutRemain": 25, // Integer, relay timeout remain in seconds
        "iRelayTimeout": 300, // Integer, relay timeout in seconds
        "sRelayDirectory": "..." // String, relay directory name
    }
}
```

#### Async Submit (ASYNC_SUBMIT)

```json
{
    "sUID": "0-12345678901234567890", // String, unique async task ID
    "iCreatedTimestamp": 1776353445000, // Integer, Unix timestamp of task creation in milliseconds
    "iUpdatedTimestamp": 1776353445000, // Integer, Unix timestamp of last task update in milliseconds
    "sAction": "FREE_UP", // String, action type passed during submission
    "sDevPath": "/dev/sda1", // String, device path of the target slot
    "sStatus": "QUEUED", // String, task status, one of: "QUEUED", "RUNNING", "SUCCEEDED", "FAILED", "CANCEL_REQUESTED", "CANCELED"
    "bDone": false, // Boolean, true if status is completed (SUCCEEDED, FAILED, CANCELED)
    "bCancelable": true, // Boolean, true if task can still be canceled
    "iProgressPercent": 0, // Integer, estimated completion progress 0-100
    "dResult": {}, // Object, final or intermediate execution result details
    "code": 0, // Integer, always present, 0 on success
    "message": "" // String, optional, present only on failure
}
```

#### Async Status (ASYNC_STATUS)

```json
{
    "lTasks": [ // Array of objects (optional), omitted if no matched tasks
        {
            "sUID": "0-12345678901234567890", // String, unique async task ID
            "iCreatedTimestamp": 1776353445000, // Integer, Unix timestamp of task creation in milliseconds
            "iUpdatedTimestamp": 1776353448000, // Integer, Unix timestamp of last task update in milliseconds
            "sAction": "FREE_UP", // String, action type
            "sDevPath": "/dev/sda1", // String, device path of the target slot
            "sStatus": "RUNNING", // String, current active status
            "bDone": false, // Boolean, true if status is completed
            "bCancelable": true, // Boolean, true if task can still be canceled
            "iProgressPercent": 42, // Integer, estimated completion progress 0-100
            "dResult": { // Object, detailed action outcome (schema varies by sAction)
                "sFileOpsType": "FREE_UP", // String (optional), "FREE_UP" or "REMOVE" for file ops
                "iRequestedCount": 10, // Integer (optional)
                "iProcessedCount": 4, // Integer (optional)
                "iSuccessCount": 4, // Integer (optional)
                "iFailedCount": 0, // Integer (optional)
                "iCode": 0, // Integer (optional), non-zero system error code if failed
                "sMessage": "" // String (optional), failure reason
            },
            "iQueuePosition": 0 // Integer (optional), 0: running, 1..N: queued, null: finished (position at time of completion)
        }
    ],
    "code": 0, // Integer, always present, 0 on success
    "message": "" // String, optional, present only on failure
}
```

#### Async Cancel (ASYNC_CANCEL)

```json
{
    "lCancelledTasks": [ // Array of objects (optional), omitted if no matched tasks
        {
            "sUID": "0-12345678901234567890", // String, unique async task ID
            "iCreatedTimestamp": 1776353445000, // Integer, Unix timestamp of task creation in milliseconds
            "iUpdatedTimestamp": 1776353449000, // Integer, Unix timestamp of last task update in milliseconds
            "sAction": "FREE_UP", // String, action type
            "sDevPath": "/dev/sda1", // String, device path of the target slot
            "sStatus": "CANCEL_REQUESTED", // String, current updated status after cancellation
            "bDone": false, // Boolean, true if status is completed
            "bCancelable": false, // Boolean, true if task can still be canceled
            "iProgressPercent": 42, // Integer, estimated completion progress 0-100
            "dResult": { // Object, detailed action outcome (schema varies by sAction)
                "sFileOpsType": "FREE_UP", // String (optional), "FREE_UP" or "REMOVE" for file ops
                "iRequestedCount": 10, // Integer (optional)
                "iProcessedCount": 4, // Integer (optional)
                "iSuccessCount": 4, // Integer (optional)
                "iFailedCount": 0 // Integer (optional)
            }
            // Note: iQueuePosition is not present in ASYNC_CANCEL responses
        }
    ],
    "code": 0, // Integer, always present, 0 on success
    "message": "" // String, optional, present only on failure
}
```

Runtime notes:

- `FORMAT` and `EJECT` are cancelable only while queued, formatting is only allowed on state is `NOT_FORMATTED_OR_FORMAT_UNSUPPORTED` or `NOT_MOUNTED`.
- `FREE_UP` and `REMOVE_FILES_OR_DIRECTORIES` are also cancelable while running.
- Accepted cancellations may still report `sStatus: "CANCEL_REQUESTED"`; the final `CANCELED` state is visible through a later `ASYNC_STATUS` request.
- Only one async task runs globally.
- Queue limit: 6 pending tasks.
- History limit: 3 completed tasks per `{sAction, sSlotDevPath}`.
- If no entries match, `lTasks` / `lCancelledTasks` are omitted and the response may be just `{ "code": 0 }`.
- Sync `REMOVE_FILES_OR_DIRECTORIES` reports aggregate success or failure through the common `code` / `message` fields only; per-path results are not returned.
- Async `REMOVE_FILES_OR_DIRECTORIES` exposes aggregate progress counts in `dResult`, but does not expose per-path result lists.

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
