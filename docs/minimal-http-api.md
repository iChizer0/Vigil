# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-10-28
Version: 2.0.0

## Rule API

### General Rule Configuration

Get or set rule enablement and the post-trigger actions.

**Path:** `.../vigil/rule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "iSchemaVersion": 2, // Integer, required, must be 2
    "bRuleEnabled": true, // Boolean, required, enable or disable the evaluation of all rules; the schedule rule takes precedence over the record rule
    "bContinuousRecordEnabled": false, // Boolean, required, record MP4 clips back to back while the schedule is open, whatever bRuleEnabled is (see below)
    "dActions": { // Object, required, the post-trigger actions
        "dPhoto": {
            "bEnabled": false, // Boolean, required
            "sFormat": "JPG", // String, required, case-sensitive, one of: "JPG", "RAW"
            "iIntervalMs": 5000 // Integer, required, positive, the delay between captures in milliseconds, write duration included
        },
        "dVideo": {
            "bEnabled": true, // Boolean, required; the format is always MP4
            "iIntervalMs": 5000 // Integer, required, positive, the delay between recordings in milliseconds, write duration included
        },
        "dSound": {
            "bEnabled": false // Boolean, required, play a sound on trigger through the host's playback callback; unavailable without one
        }
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

The payload is the version-2 record configuration (`record_config.json`). The document is always complete: `GET` returns every field, and `POST` takes the whole document back, so edit the `GET` response and post it; both use the canonical form. Unknown fields are ignored and dropped from the stored document. Only version 2 is read: an external manager migrates older files, and Vigil never rewrites a stored file it cannot read. A first start without `record_config.json` reports every action disabled.

Any combination of actions may be enabled, including none with `bRuleEnabled` true, in which case every rule event is reported and nothing else happens. Photo and Video run independently, each on its own interval, and may both publish a file for one rule event. Sound plays through the host's callback and is never queued: a trigger while one is pending or playing is dropped. A SED rule with Sound enabled can retrigger itself through the microphone; any guard time belongs to the host's playback.

`bContinuousRecordEnabled` records MP4 clips of the target length (`bVideoClipLengthSeconds` in Rule Info) back to back on the video channel whenever the schedule is open and storage and media allow, whatever `bRuleEnabled` is. Its clips are not rule events: no rule callback reports them, and their files carry `event_uid` 0. While it is set, `dVideo` keeps its stored value but no triggered video is admitted for a rule event; Photo and Sound are unaffected, and an HTTP activation then needs storage only when Photo is enabled. Switching either way finishes the running clip first, under its own identity, before the other mode's first clip starts, and after a completed triggered clip the first continuous clip also waits for the rest of that clip's interval. A clip that loses the schedule, media, or storage ends early and is kept, and the next clip starts at a key frame enqueued after it is admitted. A schedule closure, a media pause, or switching continuous recording off and on between two clips does the same, so continuous recording records nothing from a closed schedule interval or a media pause; a slot that is briefly not ready between two clips does not, and the next clip continues where the previous one ended while that key frame is still buffered. `ALWAYS_ON`, the default record rule, cannot be combined with continuous recording, even while `bRuleEnabled` is false (see Always-On Rule).

A triggered video recording starts from the oldest key frame still buffered, so it holds up to the buffer period (10 seconds) before its event. It ends on the first key frame after the target clip length has elapsed since the recording started or, if none arrives, when the key frame wait runs out. A continuous clip that follows a completed one, with no schedule closure, media pause, or switch between them, starts at the key frame that clip ended on, so the two neither overlap nor leave a gap; any other continuous clip starts at a key frame enqueued after it is admitted.

A stored record configuration, record rule, or schedule file that cannot be loaded at startup is left in place, and rules and continuous recording stay off until a valid record configuration is posted. Meanwhile `GET` reports the defaults in the unusable file's place: every action disabled, the `ALWAYS_ON` rule, or the disabled schedule, whose gate is always open. To recover, first post the unusable file's own document (the record rule or schedule configuration), then the record configuration; posting the record configuration first would turn on the defaults those `GET`s show. Every valid schedule `POST` is stored, so posting back what its `GET` shows repairs that file.

Notes:

- A missing field, a field of the wrong type, a non-positive or non-integer interval, an unknown photo format, or any other `iSchemaVersion` is rejected with `EINVAL` (`30022`). A rejected payload changes neither the runtime nor the stored file.
- While `ALWAYS_ON` is selected (the default), a payload with actions it cannot pace is rejected with `EINVAL` (see Always-On Rule); select another record rule first.
- `EAGAIN` (`30011`) means the action settings changed (a media refresh or another commit) while the document was being prepared or applied. Post it again: the document may already be stored, with the runtime still on the previous one.
- The C API reports rule events and published files through its rule and file callbacks, best-effort: the event objects are borrowed for the callback, a file callback may precede the rule callback of its event, and a rule event is followed by zero, one, or two file callbacks. The contract is stated in `include/vigil_ctypes.h`.

Capability limits:

- Vigil owns the rules, the admission, and the file lifecycle, not the devices behind the callbacks.
- A file callback that keeps failing, or files that keep failing to publish, stop recording on the storage slot. Each file channel counts its failed tasks (a failing create, write, finish, or capture callback, a writer that could not be built, or a recorded file that could not be moved into its day's directory and was removed instead), and after each one stays held through its interval plus a 3-second cooldown. After 10 failed tasks without a completed one in between, it takes the slot to `ERROR` (Storage Status), which stops the other channel and continuous recording too until the slot is disabled and enabled again: reselect it through the storage configuration after selecting another, eject and reinsert a removable device, or restart the service. A persistently failing Photo callback therefore ends Video recording on that slot as well.
- Sound plays only through the host's callback, which must return in bounded time. Vigil queues no playback and retries nothing.
- The timing in this document (the buffer period, the clip length and its key frame and input loss waits, the action intervals, and the 10 ms rule evaluation and 50 ms worker probe periods behind them) is the host logic's target, verified with fake callbacks on a development host. Real-device latency, callback duration, and the gap between clips depend on the host's media pipeline and storage, and are the integration's to measure.
- Storage calls on the slot's filesystem must return promptly: the free-space probe at each admission (on the rule thread and an HTTP activation's thread), the directory creation and rename at publication (on the slot's workers), and the filesystem sync at a disable or before an unmount (on the thread of the switch, the eject, or the removal). A hanging device stalls the threads that make them.
- On FAT a day's directory holds about 16,000 files, since a FAT directory has at most 65,536 entries and each recorded file's name takes four. Once it is full, every further file of that day fails to publish and counts as a failed task (above), so the slot reaches `ERROR` soon after. A photo every 5 seconds, as `ALWAYS_ON` takes them at the default interval, fills it at about 23:00; under another rule, one every 6 seconds or more leaves room for continuous clips too. On FAT, keep a day's files under that, or use ext4.
- The host's video sink must admit every file Vigil creates. With a single video sink the second create fails and counts like any other failure: a manual video capture that overlaps continuous recording fails the automatic clips meanwhile, and ten such failures reach the limit above in about 30 seconds (one 3-second cooldown each).
- Video frames must carry a key frame at least every 10 seconds, the buffer period. A clip's first and closing key frames are waited for at most that long, and a longer GOP cancels clips or ends them without a boundary.

### Rule Info

Get the information about the rule system.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "bReadyForNewEvent": true, // Boolean, false while a file channel holds a triggered task (PENDING, WRITING, or HOLDING); a continuous clip does not count
    "dLastRuleEvent": { // Object or null, the most recently accepted triggered file task: the held one while a channel holds one (the one accepted last when both do), otherwise the last retired one (a later continuous clip replaces the video channel's)
        "sStatus": "COMPLETED", // String, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN" (see the notes)
        "iTimestamp": 1735574400000, // Integer, the owner's event time in Unix milliseconds
        "iOwnerUID": 1735574400123 // Integer, UID of the owner that created the event (exact 64-bit value); 0 for a continuous clip, seen only in dActionStates.dVideo.dEvent
    },
    "dLastRuleEventOwner": { // Object or null, the owner of dLastRuleEvent
        "sRuleType": "TIMER", // String, rule type; "UNKNOWN" for a direct EVENT delivered without a rule
        "sRuleID": "", // String, the condition: the inference member ID, the GPIO signal (e.g. "RISING"), the TTY command, or the SED rule's sID; empty for other rules
        "sSourceID": "", // String, the inference source for inference and direct EVENT owners, the pin name for GPIO, the TTY name for TTY, otherwise empty
        "sSourceDelivery": "EVENT", // String, "FRAME" for inference frame owners, "EVENT" for every other owner
        "iTimestamp": 1735574400000, // Integer, event time in Unix milliseconds: the input's timestamp for FRAME and EVENT owners, otherwise when the condition was observed
        "iDispatchTimestamp": 1735574400000, // Integer, Unix milliseconds when Vigil created the owner; equals iTimestamp for owners without an input
        "iUID": 1735574400123 // Integer, unique owner ID (exact 64-bit value), also carried by the rule and file callbacks
    },
    "dLastDispatchedOwner": { // Object or null, the owner of the most recent dispatch, whatever the actions did with it, in the shape of dLastRuleEventOwner
        // ...
    },
    "dActionStates": { // Object, the state of each post-trigger action
        "dPhoto": {
            "bEnabled": true, // Boolean, whether the action is enabled in the general rule configuration
            "bReady": false, // Boolean, enabled and holding no task: the next event can be admitted here, subject to the gates and the storage slot
            "sState": "WRITING", // String, one of: "IDLE" (no task held), "PENDING" (accepted, not yet claimed by a worker), "WRITING", "HOLDING" (see the notes)
            "dEvent": { // Object or null, the held task's event, in the shape of dLastRuleEvent
                "sStatus": "WRITING",
                "iTimestamp": 1735574400000,
                "iOwnerUID": 1735574400123
            }
        },
        "dVideo": {
            // ... The same fields as dPhoto, where bReady is false while continuous recording is configured, plus:
            "bContinuous": false // Boolean, whether continuous recording is configured; bEnabled still reports dVideo.bEnabled as configured
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
    "bVideoClipLengthSeconds": 60, // Integer (the b prefix is kept for compatibility), target video clip length in seconds; a clip ends at the first key frame after it, so its length varies with the GOP
    "bMediaPaused": false, // Boolean, whether media capture is currently paused (e.g., due to configuration update in underlying encoder system)
    "bRecordEnabled": true, // Boolean, whether rules are enabled (bRuleEnabled of the general rule configuration); the schedule must also be open for a rule to trigger
    "dRecordSlot": {
        // ... The storage slot object of the Storage Status response for the enabled slot, whatever its readiness; null while no slot is enabled
    }
}
```

Notes:

- `INTERRUPTED`: a gate, pause, stop, settings change, or input loss stopped a task that had started; its file is kept.
- `CANCELED`: nothing was recorded and no file exists. The task was retired before a worker claimed it, the gate was already closed when the writer started, or no key frame arrived before the clip's key frame wait, gate, or end.
- `HOLDING`: the task finished and the channel still holds it, through its interval after a completed task, or its interval plus the failure cooldown after a failed one. Its file is published as the hold begins: if that fails, the file is removed, a `COMPLETED` or `INTERRUPTED` status turns `FAILED`, and the task is held as a failed one. An interrupted or canceled task is otherwise retired at once.
- A continuous clip is not a rule event: it shows under `dActionStates.dVideo` only, never in `dLastRuleEvent`.
- `dLastDispatchedOwner` moves ahead of `dLastRuleEventOwner` when the file actions rejected the event or did not apply to it.
- While continuous recording is configured, the video channel records clips without an owner (`iOwnerUID` 0) and admits no triggered video.
- `dRecordSlot.sState` says whether the slot can record, except that a slot which reached its failure limit refuses at once and shows `ERROR` from the manager's next poll.

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

The payload depends on the `sCurrentSelected` field. An activation while a file channel still holds its previous task (through the interval after its start, plus the cooldown after a failed one) is reported and triggers Sound, but admits no new file on that channel; `ALWAYS_ON` waits for the channel instead.

#### Inference Set Rule

```json
{
    "sCurrentSelected": "INFERENCE_SET", // String, one of: "SED", "INFERENCE_SET", "TIMER", "GPIO", "TTY", "HTTP", "ALWAYS_ON"
    "lInferenceSet": [
        {
            "sID": "person_detection", // String, rule ID (uniqueness not enforced but recommended)
            "lSourceFilter": ["builtin", "fall-detection"], // Optional; array of source ids to consume, empty or absent matches every source. "builtin" is the on-device inference pipeline
            "iDebounceTimes": 3, // Integer, 0 to 2147483647, the threshold of the saturating counter described below
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

`lSourceFilter` selects the result sources this rule consumes, matched against the canonical source id. Source ids are stamped by the receiving ingress from the peer identity; a producer's self-reported `source_id` is never taken as is. An absent or empty filter matches every source. Results from unlisted sources neither advance nor decay this rule's debounce state.

`iDebounceTimes` is the threshold of a saturating counter, not a count of consecutive frames. A consumed frame with a matching detection raises the counter by one, up to the threshold; a consumed frame without one lowers it by one, down to zero; and a frame activates the rule when the counter stands at the threshold afterwards. So a single non-matching frame deactivates the rule and a single matching one reactivates it, while a run of non-matching frames as long as the threshold empties the counter, and as many matching frames are needed again. Frames from unlisted sources and ticks without a frame leave the counter alone. A threshold of zero activates on every consumed frame, matching or not; a negative or oversized threshold is rejected with `EINVAL`.

Applications whose post-processing does not fit the detection or classification formats can decide on recording themselves and emit a discrete event (`INFERENCE_DELIVERY_EVENT` with no result payload) instead. Subject to rule enablement and the schedule, such an event owns the tick it is consumed on and dispatches at once, bypassing the selected rule, its filters, and the debounce counter. It is reported with rule type `UNKNOWN`, an empty rule id, the source id as `sSourceID`, delivery `EVENT`, and the input's timestamp as the event time.

#### Sound Event Rule

```json
{
    "sCurrentSelected": "SED",
    "dSED": {
        "sID": "00000000-0000-0000-0000-000000000000", // String, AcousticsLab head/model ID to match against inference head_id; empty string accepts any model
        "iConsecutiveWindowMs": 1000, // Integer, required consecutive active duration in milliseconds before activation, must be >= 0
        "lConfidenceFilter": [0.5, 1.0], // Array of number [min, max], confidence range to filter, configurable range between 0.0 and 1.0
        "lClassFilter": ["doorbell", "baby_cry"] // Array of strings, class labels to filter; an empty array matches no label, so the rule never activates
    },
    "code": 0,
    "message": ""
}
```

The rule reads AcousticsLab inference frames from the Unix socket `/dev/shm/acousticslabd-result.sock`. Each frame is a little-endian 32-bit payload length, between 1 and 65536 bytes, followed by an `acousticslab.v1.Envelope`. A frame is active when its inference carries the configured head id (any head with an empty `sID`) and a top-k entry whose label is in `lClassFilter` and whose probability lies within `lConfidenceFilter`; an envelope without an inference payload is ignored and does not end the run.

The rule activates on the first active frame at least `iConsecutiveWindowMs` after its run of active frames began, and on every active frame after that. An inactive frame ends the run, but silence does not, since the window is measured on frames rather than by a timer; with a window of 0 every active frame activates. An activation is latched until the next rule evaluation consumes it, however long that takes, and several activations before an evaluation count once. Rules are evaluated only while `bRuleEnabled` is true and the schedule is open, so an activation during a closure is reported once after reopening.

A zero or oversized frame length, a malformed payload, a frame whose bytes stop arriving for 5 seconds, or the peer closing the socket ends the connection. The rule then reconnects after a backoff that doubles from 100 ms to 5 s across connections that delivered no valid frame and returns to 100 ms once one did; while connected, it waits without limit for the next frame. The rule plays no sound itself: `dSound` in the general rule configuration does, through the host's callback.

#### Timer Rule

```json
{
    "sCurrentSelected": "TIMER",
    "dTimer": {
        "iIntervalSeconds": 60 // Integer, trigger interval in seconds, must be >= 0
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
        "sState": "FLOATING", // String, GPIO pin state, one of: "FLOATING", "PULL_UP", "PULL_DOWN" ("DISABLED" is rejected: a rule needs a readable pin)
        "sSignal": "HIGH", // String, GPIO pin signal to trigger recording, one of: "HIGH", "LOW", "RISING", "FALLING"
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
        "sCommand": "RECORD" // String, non-empty command string to activate recording
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

Rule events from a TTY rule carry the command as the rule ID and the TTY name as the source ID.

#### HTTP Rule

The HTTP rule needs no further configuration: select it to trigger recording through the HTTP Rule Activation API.

```json
{
    "sCurrentSelected": "HTTP",
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

#### Always-On Rule

The always-on rule needs no further configuration. When selected, it triggers every `iIntervalMs` of the enabled file action (see General Rule Configuration): the next recording starts no earlier than the interval after the previous one started, and only once the previous write has finished.

Because it paces one file action, selecting `ALWAYS_ON` requires the stored general rule configuration to satisfy an invariant: `dSound` and `bContinuousRecordEnabled` false, at most one of `dPhoto` and `dVideo` enabled, and exactly one while `bRuleEnabled` is true. The first-start configuration satisfies it. The record rule `POST` that selects `ALWAYS_ON` is rejected with `EINVAL` otherwise, and while it is selected the general rule configuration `POST` is held to the same limit, so simplify the actions or select another rule first. Both checks share one lock, so neither the order of the posts nor concurrent posts can bypass it. At startup a stored pair that violates it keeps rules and continuous recording off (`bRuleEnabled` and `bContinuousRecordEnabled` read false), with both files left in place.

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

- Returns `EPERM` if rules are disabled, the schedule is closed, or the selected rule is not `HTTP`.
- Returns `EBUSY` if a file action is enabled and the storage slot is not ready for writing. A rule-only or Sound-only activation needs no storage.
- Returns `EBUSY` if a file action is enabled and no file channel accepted the event: every enabled channel still holds a task, the slot has no headroom or turned unusable after the precheck, or a commit changed the settings meanwhile. The event is still reported and Sound is triggered. A gate that closed after the precheck (the rule flag, the schedule, or the media pause, which the precheck does not read) also answers `EBUSY`, and refuses Sound too.
- A refused activation leaves no pending event behind: the next activation is admitted afresh.
- A Sound-only activation answers 0 once its owner is dispatched, even when the Sound trigger was dropped: one was pending or playing, the media pause refused it, or a tick of the rule loop that had read rules as disabled canceled it right after a commit enabled them.

## Storage API

### Storage Configuration

Get or set the storage configuration.

**Path:** `.../vigil/storage/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "dSelectSlotToEnable": { // Object, set to null to disable the enabled slot or enable none
        "sByDevPath": "/dev/sda1", // String, device path to filter the target enabled slot, empty string to skip this filter
        "sByUUID": "550e8400-e29b-41d4-a716-446655440000" // String, UUID to filter the target enabled slot, empty string to skip this filter
    },
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```

A selector that matches several slots, such as a UUID shared by cloned cards, enables one of them (the one already enabled, if any), and each matching slot reports `bSelected`.

The internal storage root (for example `/userdata`, `root:root 01777`) and any bind alias of it belong to the factory image: their mode keeps admin and application write access while protecting root-owned configuration entries, and Vigil never changes their owner or mode. Vigil sets the mounted root of every other slot (any device but `internal_device_path` of the storage C API) to `1000:1000`, mode `0777`. Recording subdirectories are unaffected.

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
            "bInternal": false, // Boolean, whether the storage is the internal partition
            "sLabel": "MyDrive", // String (optional), volume label, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "sUUID": "550e8400-e29b-41d4-a716-446655440000", // String (optional), volume UUID, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "sType": "ext4", // String (optional), filesystem type, null if not available or not formatted yet, may not exist if eState < NOT_MOUNTED
            "bSelected": true, // Boolean, whether this slot is the target enabled slot according to the current configuration
            "bEnabled": true, // Boolean, whether this slot is currently enabled
            "bDirty": false, // Boolean (optional), whether the filesystem is marked dirty and may need fsck; null if not checked or unknown
            "bSyncing": false, // Boolean, whether this slot is currently indexing its files
            "bWriting": false, // Boolean, whether this slot currently has active writing tasks (e.g., recording videos, writing images)
            "bRotating": false, // Boolean, whether this slot is currently performing quota rotation
            "eState": 8, // Integer, state of the slot (-1: ERROR, 1: NOT_FORMATTED_OR_FORMAT_UNSUPPORTED, 2: FORMATTING, 3: NOT_MOUNTED, 4: FILESYSTEM_CHECKING, 5: MOUNTED, 6: CONFIGURED, 7: INDEXING, 8: READY, 9: SATURATED)
            "sState": "READY", // String, human-readable state ("ERROR", "NOT_FORMATTED_OR_FORMAT_UNSUPPORTED", "FORMATTING", "NOT_MOUNTED", "FILESYSTEM_CHECKING", "MOUNTED", "CONFIGURED", "INDEXING", "READY", "SATURATED")
            "iStatsSizeBytes": 1000000000, // Integer, total size of the partition, not accurate if eState < MOUNTED
            "iStatsFreeBytes": 500000000, // Integer, free space of the partition, not accurate if eState < MOUNTED
            "iQuotaMinimumRecommendBytes": 200000000, // Integer, minimum recommended quota limit; a lower limit may not work well
            "iQuotaPreservedBytes": 0, // Integer, space preserved for system use; a quota limit above (iStatsSizeBytes - iQuotaPreservedBytes) may not be fully usable
            "iQuotaUsedBytes": 10000000, // Integer (optional), space used under the quota, null if eState < CONFIGURED, not accurate if eState < READY
            "iQuotaLimitBytes": 100000000, // Integer, configured quota limit, -1 means no limit, not accurate or the default if eState < CONFIGURED
            "bQuotaRotate": true // Boolean, whether quota rotation is enabled, not accurate or the default if eState < CONFIGURED
        },
        // ... More storage slots, dynamically updated
    ],
    "sDataDirPath": "DCIM" // String, path of the record data directory under each storage mount point
}
```

Notes:

- The status changes at runtime (devices, states, space, activity); poll it to follow the slots.
- The used quota space (`iQuotaUsedBytes`) may be slightly larger than the sum of the recorded files because of filesystem overhead.
- `iQuotaPreservedBytes` keeps the filesystem from filling up completely, for system stability.
- `iQuotaMinimumRecommendBytes` is a guideline; a quota limit below it may perform poorly or not work at all.
- The quota limit and rotation take effect only once the slot is `CONFIGURED` or later, and are stored per slot: changing one slot's settings does not affect the others.
- The quota limit is not always fully usable: when the free space on the device is less than `iQuotaLimitBytes` minus `iQuotaUsedBytes`, the free space is the limit, and with rotation enabled the oldest files are deleted when the usable space runs out.
- Vigil mounts and unmounts the slots as needed, and formats one only on a `FORMAT` request. It enables one slot at a time: enabling a slot disables the one enabled before.
- Changing many files directly under the record data directory of an enabled slot can interfere with the storage system; use the Storage Control API to remove files.
- A slot takes time to move between states; watch `eState` to see when it is ready.
- `bDirty` means the filesystem is marked dirty and may need a check (fsck) before it can be used again, for example after the device was removed without an unmount or after write errors. A dirty filesystem is not mounted automatically until it is repaired; enabling the slot attempts the repair (`fsck -p`).
- `SATURATED` means free space is critically low and new recordings stop. With quota rotation off the state holds until files are removed (`REMOVE_FILES_OR_DIRECTORIES` or `FREE_UP`), the quota limit is raised, rotation is turned on, or the configured file actions' critical watermark falls (one file action instead of two, or a smaller format); a recording accepted just before the state changed is canceled rather than left `PENDING` until then. With rotation on it waits for the rotation, which brings the slot back to `READY`.

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
    "sTaskUID": "0-12345678901234567890", // String (optional), queried by ASYNC_STATUS / ASYNC_CANCEL; currently a non-empty value is ignored and an empty one matches nothing
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

- Async tasks (`ASYNC_*`) are supported only by `FORMAT`, `FREE_UP`, `EJECT`, and `REMOVE_FILES_OR_DIRECTORIES`.
- `CONFIG`, `RELAY`, `RELAY_STATUS`, and `UNRELAY` are sync only.

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

- `FORMAT` and `EJECT` can be canceled only while queued. Formatting is allowed only in `NOT_FORMATTED_OR_FORMAT_UNSUPPORTED` or `NOT_MOUNTED`.
- `EJECT` answers after the slot is disabled, its filesystem synced, the unmount attempted, and the slot dropped from the storage status; a sync or unmount failure is logged, not reported. A device pulled and reinserted while the eject runs comes back as a new slot, which the status lists after the answer.
- A `FREE_UP` or `REMOVE_FILES_OR_DIRECTORIES` of the slot that is still running holds its filesystem, so an eject only detaches the mount and the filesystem stays busy until that request returns. Let such a request finish, or cancel it, before ejecting.
- `FREE_UP` and `REMOVE_FILES_OR_DIRECTORIES` can also be canceled while running.
- An accepted cancellation may still report `sStatus: "CANCEL_REQUESTED"`; a later `ASYNC_STATUS` shows the final `CANCELED`.
- One async task runs at a time across all slots. The queue holds up to 6 pending tasks, and the history keeps the last 3 completed tasks per `{sAction, sSlotDevPath}`.
- When no task matches, `lTasks` and `lCancelledTasks` are omitted and the response may be just `{ "code": 0 }`.
- `REMOVE_FILES_OR_DIRECTORIES` reports aggregate results only: the sync form through `code` and `message`, the async form as progress counts in `dResult`; per-path results are not returned.

## Capture API

The Capture API controls media capture by hand, independently of the rule-based recording, with two exceptions: the `ALWAYS_ON` rule opens no clip while a capture (or any other media operation) is active, and a host with a single video sink cannot run a video capture and a recording at once (see the capability limits).

### Capture Status

Get the current capture status.

**Path:** `.../vigil/capture/status`

**Method:** `GET`

**Response Body:**

```json
{
    "dLastCapture": { // Object (optional), the active capture if one is in progress, otherwise the last completed one, null if none
        "sID": "194e8b2c0000194e8b2c0001", // String, unique capture event ID
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, output directory path
        "sFormat": "MP4", // String, capture format ("MP4", "JPG", "RAW")
        "iVideoLengthSeconds": 60, // Integer, optional, present only for video captures with finite length
        "sStatus": "WRITING", // String, write event status, one of: "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", "UNKNOWN" (see the notes)
        "iTimestamp": 1736870400000, // Integer, Unix timestamp in milliseconds
        "sFileName": "Capture 20260114 143000 123456.mp4" // String, generated file name
    },
    "bReadyToStartNew": true, // Boolean, whether a new capture can be started (false while a capture is in progress)
    "bStopRequested": false // Boolean, whether a stop capture request is pending
}
```

Notes:

- `PENDING`: accepted and not yet running, also while media is paused.
- `COMPLETED`: an image was captured, or a video reached its length or its stop and ended on the next key frame or, if none arrived, when the key frame wait ran out.
- `FAILED`: a callback failed, the writer could not be built, or the output filesystem had no headroom; the capture leaves no file.
- `INTERRUPTED`: a pause, the service stopping, input loss, or free space below the critical watermark (checked every 60 seconds) ended a video after it started; the file is kept.
- `CANCELED`: nothing was recorded and no file exists, because media was paused or no key frame arrived within the key frame wait or before the capture was stopped.

### Start Capture

Start a new capture session.

**Path:** `.../vigil/capture/start`

**Method:** `POST`

**Request Payload:**

```json
{
    "sOutput": "/mnt/sdcard/DCIM/20260114", // String, absolute path under storage mount point (directory or full file path)
    "sFormat": "MP4", // String, capture format: "MP4" (video), "JPG" (image), "RAW" (image)
    "iVideoLengthSeconds": 60 // Integer, optional, video clip length in seconds; omit to record until stopped
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

- Returns `EBUSY` if a capture is already in progress: one capture runs at a time.
- The output path must be absolute and start with `/mnt` or the configured internal mount point. The check compares strings only: it normalizes nothing and does not look for a mounted slot, so a path through `..`, or one that merely shares the prefix, passes, and the capture is written there, replacing a file of that name.
- A missing output directory is created (and stays if the capture then fails). The file is written under its name prefixed with a dot and renamed to `sFileName` when the capture ends; a stale file of the dot-prefixed name is removed first.
- A capture is admitted against the same per-filesystem in-flight budget as automatic recording. When the output filesystem's free space, minus what admitted tasks may still write there, is at or below the capture's critical watermark, the capture fails before any callback (`FAILED`, no file). While it runs, its own estimate is in flight on that filesystem and counts against automatic admission there.
- Nothing runs while media is paused: the capture waits as `PENDING` and runs once media resumes.

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

- Returns `ENOENT` if no capture is in progress, and `EALREADY` if a stop is already in progress.
- A stop ends a video capture as `COMPLETED` on the next key frame, the frames until then included, or when the key frame wait runs out without one; a clip that has not yet seen its first key frame is `CANCELED`. An image capture that has not run yet is still taken. The request is cleared with the capture.

## Files API

### Files Fetch

Fetch file contents through the relay.

**Path:** `.../vigil/relay/<relay_uuid>/<file_path>`

**Method:** `GET`

**Description:** To access files, a client first requests a `RELAY` action through the Storage Control API. This creates a temporary relay directory (a symlink) named by a UUID, returned as `dRelayStatus.sRelayDirectory`; the client can then fetch files under it.

**Example:** `.../vigil/relay/550e8400-e29b-41d4-a716-446655440000/DCIM/20251016/event.mp4`

Notes:

- The relay directory times out 300 seconds after the last `RELAY` request; send `RELAY` again to keep it open. Fetching files does not refresh it.
- Use the `UNRELAY` action to close the relay explicitly.
- When the host provides `thumbnail_generate_cb`, a recorded video is published with a thumbnail: `/path/to/video.mp4` -> `/path/to/.thumb/video.mp4.thumb.jpg`.

## Error Handling

Every `POST` response carries a `code` field, 0 on success; a `GET` response carries one only on failure. A non-zero `code` is error_base (30000) plus abs(posix_errno), and a `message` string may be included. Request bodies are limited to 1 MiB; a larger body is rejected before parsing (the C API returns `EFAULT` with no JSON body).

```json
{
    "code": 0, // Integer, always present in POST, 0 on success, otherwise error_base + abs(posix_errno)
    "message": "" // String, optional, present only on failure
}
```
