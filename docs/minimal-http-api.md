# Vigil Record Minimal HTTP API Specification

Created: 2025-10-16
Revised: 2026-10-29
Version: 2.0.0

Every `POST` answers with `code`, 0 on success, else 30000 plus the POSIX errno, and `message` on failure; a `GET` carries them only on failure (see Error Handling). The examples show every field once with its type and meaning.

## Rule API

### General Rule Configuration

Get or set rule enablement and the post-trigger actions.

**Path:** `.../vigil/rule/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "iSchemaVersion": 2, // Integer, required, must be 2
    "bRuleEnabled": true, // Boolean, required, evaluate the rules; the schedule gates them as well
    "bContinuousRecordEnabled": false, // Boolean, required, record MP4 clips back to back while the schedule is open, whatever bRuleEnabled is
    "dActions": { // Object, required, the post-trigger actions
        "dPhoto": {
            "bEnabled": false, // Boolean, required
            "sFormat": "JPG", // String, required, case-sensitive, "JPG" or "RAW"
            "iIntervalMs": 5000 // Integer, required, positive, in milliseconds: the delay between captures, write included
        },
        "dVideo": {
            "bEnabled": true, // Boolean, required; the format is MP4
            "iIntervalMs": 5000 // Integer, required, positive, in milliseconds: the delay between recordings, write included
        },
        "dSound": {
            "bEnabled": false, // Boolean, required, play a sound on trigger through the host's playback callback; unavailable without one
            "sFilePath": "" // String, optional, the absolute path of the audio file to play; empty or absent plays the host's own sound, and Vigil checks only that it is absolute
        }
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

The payload is the version-2 `record_config.json`, always complete and in the canonical form: `GET` returns every field and `POST` takes the whole document back, so edit the `GET` response and post it. Unknown fields are ignored and dropped from the stored document. Only version 2 is read; an external manager migrates older files, and Vigil never rewrites a file it cannot read. A first start without the file reports every action disabled.

Any combination of actions may be enabled, including none with `bRuleEnabled` true, when every rule event is reported and nothing else happens. Photo and Video run independently on their own intervals and may both publish a file for one rule event. Sound plays through the host's callback, which receives `sFilePath`, and is never queued: a trigger while one is pending or playing is dropped. A SED rule with Sound enabled can retrigger itself through the microphone; any guard time belongs to the host's playback.

`bContinuousRecordEnabled` records clips of the target length (`bVideoClipLengthSeconds` in Rule Info) back to back on the video channel whenever the schedule is open and storage and media allow. Its clips are not rule events: no rule callback reports them and their files carry `event_uid` 0, but a rule event dispatched while a clip is pending or being written lists the clip's file id in `file_ids`, as it lists a triggered clip accepted for it or already in progress, so a host can find a trigger's footage (see `include/vigil_ctypes.h`).

While it is set, `dVideo` keeps its stored value but no triggered video is admitted; Photo and Sound are unaffected, and an HTTP activation needs storage only when Photo is enabled. `ALWAYS_ON`, the default record rule, cannot be combined with it, even while `bRuleEnabled` is false (see Always-On Rule).

Switching either way finishes the running clip under its own identity before the other mode's first clip starts, and after a completed triggered clip the first continuous clip also waits out the rest of that clip's interval.

A clip that loses the schedule, media, or storage ends early and is kept, and the next clip starts at a key frame enqueued after it is admitted. A schedule closure, a media pause, or switching continuous recording off and on between two clips does the same, so nothing is recorded from a closed interval or a pause. A slot briefly not ready between two clips is different: the next clip continues where the previous one ended while that key frame is still buffered.

A clip checks the schedule and the pause as it starts and between frames, so it still writes the frames that arrive as a closure or pause begins or ends, and it keeps writing through a closure, or a pause the host does not wait for, that ends before its next check; the next clip then starts fresh.

A triggered video recording starts from the oldest buffered key frame, so it holds up to the buffer period (10 seconds) before its event, and ends on the first key frame after the target length has elapsed since the recording started or, if none arrives, when the key frame wait runs out. A continuous clip following a completed one with no closure, pause, or switch between them starts at the key frame that clip ended on, with neither overlap nor gap; any other continuous clip starts at a key frame enqueued after its admission.

A stored record configuration, record rule, or schedule file that cannot be loaded at startup stays in place, and rules and continuous recording stay off until a valid record configuration is posted; meanwhile `GET` reports the defaults in its place: every action disabled, the `ALWAYS_ON` rule, or the disabled schedule, whose gate is always open. Recover by posting the unusable file's own document (the record rule or schedule configuration) first, then the record configuration; the other order would turn on the defaults those `GET`s show. Every valid schedule `POST` is stored, so posting back what its `GET` shows repairs that file.

Notes:

- `EINVAL` (`30022`): a missing field (`sFilePath` excepted), a wrong type, a non-positive or non-integer interval, an unknown photo format, a relative `sFilePath`, or another `iSchemaVersion`. A rejected payload changes neither the runtime nor the stored file.
- `EINVAL` also while `ALWAYS_ON` is selected (the default) and the payload has actions it cannot pace (see Always-On Rule); select another record rule first.
- `EAGAIN` (`30011`): the action settings changed (a media refresh or another commit) while the document was being prepared or applied. Post it again; the document may already be stored, with the runtime still on the previous one.
- The C API reports rule events and published files through its callbacks, best-effort: the objects are borrowed for the call, a file callback may precede the rule callback of its event, and a rule event is followed by zero, one, or two file callbacks. Every automatic file has a file id, carried by its file callback and by rule info as `iFileID`, and a rule event lists in `file_ids` the file of each channel that has one at its dispatch: the one accepted for it, else the one already pending or being written. The contract is in `include/vigil_ctypes.h`.

Capability limits:

- Vigil owns the rules, the admission, and the file lifecycle, not the devices behind the callbacks.
- A file callback that keeps failing, or files that keep failing to publish, stop recording on the slot. Each file channel counts its failed tasks (a failing create, write, finish, or capture callback, a writer that could not be built, or a recorded file that could not be moved into its day's directory and was removed instead) and holds through its interval plus a 3-second cooldown after each. After 10 failed tasks with no completed one between, the slot goes to `ERROR` (Storage Status), which stops the other channel and continuous recording too until the slot is disabled and enabled again: reselect it through the storage configuration after selecting another, eject and reinsert a removable device, or restart the service. A persistently failing Photo callback therefore ends Video recording on that slot as well.
- Sound plays only through the host's callback, which must return in bounded time; Vigil queues no playback and retries nothing.
- The timing in this document (the buffer period, the clip length and its key frame and input loss waits, the action intervals, and the 10 ms rule evaluation and 50 ms worker probe periods behind them) is the host logic's target, verified with fake callbacks on a development host; real-device latency, callback duration, and the gap between clips depend on the host's media pipeline and storage, and are the integration's to measure.
- Storage calls on the slot's filesystem must return promptly: the free-space probe at each admission (on the rule thread or an HTTP activation's), the directory creation and rename at publication (on the slot's workers), and the sync at a disable or before an unmount (on the thread of the switch, the eject, or the removal). A hanging device stalls those threads.
- On FAT a day's directory holds about 16,000 files: a FAT directory has at most 65,536 entries and each recorded file's name takes four. Once it is full, every further file of that day fails to publish and counts as a failed task, so the slot reaches `ERROR` soon after. A photo every 5 seconds, the `ALWAYS_ON` default, fills it at about 23:00; under another rule one every 6 seconds or more leaves room for continuous clips too. Keep a day's files under that on FAT, or use ext4.
- The host's video sink must admit every file Vigil creates. With a single sink the second create fails and counts like any other failure: a manual video capture overlapping continuous recording fails the automatic clips meanwhile, and ten such failures reach the limit in about 30 seconds (one 3-second cooldown each).
- Video frames must carry a key frame at least every 10 seconds, the buffer period: a clip's first and closing key frames are waited for at most that long, and a longer GOP cancels clips or ends them without a boundary.

### Rule Info

Get the information about the rule system.

**Path:** `.../vigil/rule/info`

**Method:** `GET`

**Response Body:**

```json
{
    "bReadyForNewEvent": true, // Boolean, false while a file channel holds a triggered task (PENDING, WRITING, or HOLDING); a continuous clip does not count
    "dLastRuleEvent": { // Object or null, the most recently accepted triggered file task: the held one while a channel holds one (the later accepted when both do), else the last retired one; a continuous clip is never shown but, once retired, takes the video channel's place in that choice
        "sStatus": "COMPLETED", // String, "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", or "UNKNOWN" (see the notes)
        "iTimestamp": 1735574400000, // Integer, the owner's event time, Unix milliseconds; for a continuous clip, when it was admitted
        "iOwnerUID": 1735574400123, // Integer, the owner's UID (exact 64-bit value); 0 for a continuous clip, seen only under dActionStates.dVideo.dEvent
        "iFileID": 1735574400124 // Integer, the file's identity (exact 64-bit value); its file callback carries it as file_id if the file is published, and the rule events dispatched while the task was PENDING or WRITING list it
    },
    "dLastRuleEventOwner": { // Object or null, the owner of dLastRuleEvent
        "sRuleType": "TIMER", // String, the rule type, one of the sCurrentSelected values of Record Rule Configuration; "UNKNOWN" for a direct EVENT delivered without a rule
        "sRuleID": "", // String, the condition: the inference member ID, the GPIO signal (e.g. "RISING"), the TTY command, or the SED rule's sID; empty for other rules
        "sSourceID": "", // String, the inference source for inference and direct EVENT owners, the pin name for GPIO, the TTY name for TTY; otherwise empty
        "sSourceDelivery": "EVENT", // String, "FRAME" for inference frame owners, "EVENT" for every other owner
        "iTimestamp": 1735574400000, // Integer, the event time, Unix milliseconds: the input's timestamp for FRAME and direct EVENT owners (sRuleType "UNKNOWN"), else when the condition was observed
        "iDispatchTimestamp": 1735574400000, // Integer, when Vigil created the owner, Unix milliseconds; equals iTimestamp for every owner but FRAME and direct EVENT ones
        "iUID": 1735574400123 // Integer, the owner's unique ID (exact 64-bit value), carried by the rule callback as uid and by its files' callbacks as event_uid
    },
    "dLastDispatchedOwner": { // Object or null, the owner of the most recent dispatch whatever the actions did with it, in the shape of dLastRuleEventOwner
    },
    "dActionStates": { // Object, the state of each post-trigger action
        "dPhoto": {
            "bEnabled": true, // Boolean, enabled in the general rule configuration
            "bReady": false, // Boolean, enabled and holding no task: the next event can be admitted here, subject to the gates and the storage slot
            "sState": "WRITING", // String, "IDLE" (no task held), "PENDING" (accepted, not yet claimed by a worker), "WRITING", or "HOLDING" (see the notes)
            "dEvent": { // Object or null, the held task's event, in the shape of dLastRuleEvent
            }
        },
        "dVideo": { // The fields of dPhoto, with bReady false while continuous recording is configured, plus:
            "bContinuous": false // Boolean, continuous recording is configured; bEnabled still reports dVideo.bEnabled as stored
        },
        "dSound": {
            "bEnabled": false, // Boolean, enabled in the general rule configuration
            "bReady": false, // Boolean, enabled, available, and idle: the next event can trigger playback, subject to the gates
            "sState": "UNAVAILABLE" // String, "UNAVAILABLE" (no playback callback), "IDLE", "PENDING" (triggered, not yet playing), "PLAYING", or "STOPPING" (the service is shutting down)
        }
    },
    "dAvailableGPIOs": { // Object, the GPIO pins the host offers, by name
        "GPIO_01": {
            "iNum": 1, // Integer, the GPIO number
            "sState": "FLOATING", // String or null, "DISABLED", "FLOATING", "PULL_UP", or "PULL_DOWN"; null when the state cannot be read
            "lCapabilities": ["FLOATING", "PULL_UP", "PULL_DOWN"], // Array of the input modes the pin supports, whatever the peripheral circuit; the example lists every possible value
            "sLevel": "HIGH" // String or null, "HIGH" or "LOW"; null when the level cannot be read
        }
    },
    "dAvailableTTYs": { // Object, the TTYs the host offers, by name
        "ttyS0": {
            "sSocketPath": "/tmp/ttyS0.sock", // String, the Unix socket the rule connects to
            "iBufferSize": 1024 // Integer, the read buffer in bytes; a command longer than it is refused
        }
    },
    "bVideoClipLengthSeconds": 60, // Integer (the b prefix is kept for compatibility), the target clip length in seconds; a clip ends at the first key frame after it, so its length varies with the GOP
    "bMediaPaused": false, // Boolean, media capture is paused (for example while the host reconfigures its encoder)
    "bRecordEnabled": true, // Boolean, bRuleEnabled of the general rule configuration; the schedule must also be open for a rule to trigger
    "dRecordSlot": { // Object or null, the slot object of Storage Status for the enabled slot, whatever its readiness; null while no slot is enabled
    }
}
```

Notes:

- `INTERRUPTED`: a gate, pause, stop, settings change, or input loss stopped a task that had started; its file is kept.
- `CANCELED`: nothing was recorded and no file exists. The task was retired before a worker claimed it, the gate was already closed when the writer started, or no key frame arrived before the clip's key frame wait, gate, or end.
- `HOLDING`: the task finished and the channel still holds it, through its interval after a completed task, or its interval plus the failure cooldown after a failed one. Its file is published as the hold begins; if that fails, the file is removed, a `COMPLETED` or `INTERRUPTED` status turns `FAILED`, and the task is held as a failed one. An interrupted or canceled task is otherwise retired at once.
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
    "bEnabled": true, // Boolean; a disabled schedule is always open
    "lActiveWeekdays": [ // Array of open periods, in local time
        {
            "sStart": "Mon 09:00:00", // String, "Day HH:MM:SS": a case-sensitive short weekday (Sun, Mon, Tue, Wed, Thu, Fri, Sat) and an ISO 8601 time
            "sEnd": "Mon 18:00:00" // String, the same form; 24:00:00 is the end of the day, so {"sStart": "Mon 00:00:00", "sEnd": "Sun 24:00:00"} is the full week
        }
    ],
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

### Record Rule Configuration

Get or set the record rule (trigger) configuration.

**Path:** `.../vigil/rule/record/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

The payload depends on `sCurrentSelected`. An activation while a file channel still holds its previous task (through the interval after its start, plus the cooldown after a failed one) is reported and triggers Sound, but admits no new file on that channel; `ALWAYS_ON` waits for the channel instead.

#### Inference Set Rule

```json
{
    "sCurrentSelected": "INFERENCE_SET", // String, "SED", "INFERENCE_SET", "TIMER", "GPIO", "TTY", "HTTP", or "ALWAYS_ON"
    "lInferenceSet": [ // Array of member rules, ORed together
        {
            "sID": "person_detection", // String, the member's ID; uniqueness is recommended, not enforced
            "lSourceFilter": ["builtin", "fall-detection"], // Array of strings, optional, the source ids to consume; empty or absent matches every source, and "builtin" is the on-device pipeline
            "iDebounceTimes": 3, // Integer, 0 to 2147483647, the threshold of the saturating counter (below)
            "lConfidenceFilter": [0.5, 1.0], // Array [min, max], each within 0.0 to 1.0
            "lClassFilter": ["person", "car"], // Array of strings, the class names that match
            "lRegionFilter": [ // Array, optional, regions ORed together; empty means no region filtering
                {
                    "lPolygon": [ // Array of [x, y] points within 0.0 to 1.0, origin at the top left, in clockwise or counterclockwise order
                        [0.0, 0.0], [0.5, 0.0], [0.5, 0.5], [0.0, 0.5]
                    ]
                }
            ]
        }
    ],
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

`lSourceFilter` selects the result sources this rule consumes, matched against the canonical source id, which the receiving ingress stamps from the peer identity; a producer's self-reported `source_id` is never taken as is. Results from unlisted sources neither advance nor decay the debounce counter.

`iDebounceTimes` is the threshold of a saturating counter, not a count of consecutive frames: a consumed frame with a matching detection raises it by one up to the threshold, one without lowers it by one down to zero, and a frame activates the rule when the counter stands at the threshold afterwards. So one non-matching frame deactivates and one matching frame reactivates, while a run of non-matching frames as long as the threshold empties the counter and as many matching frames are needed again. Unlisted sources and ticks without a frame leave the counter alone. A threshold of zero activates on every consumed frame, matching or not; a negative or oversized one is rejected with `EINVAL`.

An application whose post-processing fits neither the detection nor the classification format can decide on recording itself and emit a discrete event (`INFERENCE_DELIVERY_EVENT` with no result payload). Subject to rule enablement and the schedule, it owns the tick it is consumed on and dispatches at once, bypassing the selected rule, its filters, and the counter, reported with rule type `UNKNOWN`, an empty rule id, the source id as `sSourceID`, delivery `EVENT`, and the input's timestamp as the event time.

#### Sound Event Rule

```json
{
    "sCurrentSelected": "SED",
    "dSED": {
        "sID": "00000000-0000-0000-0000-000000000000", // String, the AcousticsLab head (model) ID to match against the inference head_id; empty accepts any
        "iConsecutiveWindowMs": 1000, // Integer, at least 0, the active run required before activation, in milliseconds
        "lConfidenceFilter": [0.5, 1.0], // Array [min, max], each within 0.0 to 1.0
        "lClassFilter": ["doorbell", "baby_cry"] // Array of strings, the labels that match; empty matches none, so the rule never activates
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

The rule reads AcousticsLab inference frames from the Unix socket `/dev/shm/acousticslabd-result.sock`: each frame is a little-endian 32-bit payload length, 1 to 65536 bytes, then an `acousticslab.v1.Envelope`. A frame is active when its inference carries the configured head id (any, with an empty `sID`) and a top-k entry whose label is in `lClassFilter` with a probability within `lConfidenceFilter`; an envelope without an inference payload is ignored and does not end the run.

The rule activates on the first active frame at least `iConsecutiveWindowMs` after its run of active frames began, and on every active frame after that. An inactive frame ends the run, silence does not, since the window is measured on frames rather than by a timer, and a window of 0 activates on every active frame. An activation is latched until the next rule evaluation consumes it, however long that takes, and several before an evaluation count once. Rules are evaluated only while `bRuleEnabled` is true and the schedule is open, so an activation during a closure is reported once after reopening.

A zero or oversized frame length, a malformed payload, a frame whose bytes stop arriving for 5 seconds, or the peer closing the socket ends the connection. The rule reconnects after a backoff doubling from 100 ms to 5 s across connections that delivered no valid frame, back to 100 ms once one did, and waits without limit for the next frame while connected. The rule plays no sound itself: `dSound` does, through the host's callback.

#### Timer Rule

```json
{
    "sCurrentSelected": "TIMER",
    "dTimer": {
        "iIntervalSeconds": 60 // Integer, at least 0, the trigger interval in seconds
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

#### GPIO Rule

```json
{
    "sCurrentSelected": "GPIO",
    "dGPIO": {
        "sName": "GPIO_01", // String, the pin name; or "iNum", the pin number, instead
        "sState": "FLOATING", // String, "FLOATING", "PULL_UP", or "PULL_DOWN"; "DISABLED" is rejected, a rule needs a readable pin
        "sSignal": "HIGH", // String, the trigger: "HIGH", "LOW", "RISING", or "FALLING"
        "iDebounceDurationMs": 100 // Integer, at least 0, the debounce in milliseconds
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

A GPIO rule's events (the rule callback and `dLastRuleEventOwner`) carry the signal as the rule ID and the pin name as the source ID.

#### TTY Rule

```json
{
    "sCurrentSelected": "TTY",
    "dTTY": {
        "sName": "ttyS0", // String, the TTY name
        "sCommand": "RECORD" // String, non-empty, the command that triggers
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

A TTY rule's events carry the command as the rule ID and the TTY name as the source ID. The rule connects to the TTY's socket when selected and tries again on each evaluation while not connected; a peer that is not accepting connections blocks neither the selection nor the evaluation.

#### HTTP Rule

No further configuration: select it and trigger through HTTP Rule Activation.

```json
{
    "sCurrentSelected": "HTTP",
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

#### Always-On Rule

No further configuration. Selected, it triggers every `iIntervalMs` of the enabled file action (General Rule Configuration): the next recording starts no earlier than the interval after the previous one started, and only once the previous write has finished.

Because it paces one file action, selecting `ALWAYS_ON` requires the stored general rule configuration to satisfy an invariant: `dSound` and `bContinuousRecordEnabled` false, at most one of `dPhoto` and `dVideo` enabled, and exactly one while `bRuleEnabled` is true; the first-start configuration satisfies it. The record rule `POST` selecting `ALWAYS_ON` is otherwise rejected with `EINVAL`, and while it is selected the general rule configuration `POST` is held to the same limit, so simplify the actions or select another rule first; both checks share one lock, so neither the order of the posts nor concurrent posts bypass it. At startup a stored pair that violates it keeps rules and continuous recording off (`bRuleEnabled` and `bContinuousRecordEnabled` read false), with both files left in place.

```json
{
    "sCurrentSelected": "ALWAYS_ON",
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

### HTTP Rule Activation

Trigger the selected HTTP rule; no payload.

**Path:** `.../vigil/rule/http_rule/activate`

**Method:** `POST`

**Response Body:**

```json
{
    "code": 0, "message": "" // see Error Handling
}
```

Notes:

- `EPERM`: rules are disabled, the schedule is closed, or the selected rule is not `HTTP`.
- `EBUSY`: a file action is enabled and the storage slot is not ready for writing; a rule-only or Sound-only activation needs no storage.
- `EBUSY` also when a file action is enabled and no file channel accepted the event: every enabled channel still holds a task, the slot has no headroom or turned unusable after the precheck, or a commit changed the settings meanwhile. The event is still reported and Sound is triggered. A gate that closed after the precheck (the rule flag, the schedule, or the media pause, which the precheck does not read) answers `EBUSY` too, and refuses Sound as well.
- A refused activation leaves no pending event behind; the next one is admitted afresh.
- A Sound-only activation answers 0 once its owner is dispatched, even when the Sound trigger was dropped: one was pending or playing, the media pause refused or canceled it, or a tick of the rule loop that had read rules as disabled canceled it right after a commit enabled them.

## Storage API

### Storage Configuration

Get or set the storage configuration.

**Path:** `.../vigil/storage/config`

**Method:** `GET` or `POST`

**Response Body for `GET` or Request Payload for `POST`:**

```json
{
    "dSelectSlotToEnable": { // Object or null; null disables the enabled slot and enables none
        "sByDevPath": "/dev/sda1", // String, the device path to match; empty skips this filter
        "sByUUID": "550e8400-e29b-41d4-a716-446655440000" // String, the UUID to match; empty skips this filter
    },
    "code": 0, "message": "" // POST response only, see Error Handling
}
```

A selector that matches several slots, such as a UUID shared by cloned cards, enables one of them (the one already enabled, if any), and each matching slot reports `bSelected`.

The internal storage root (for example `/userdata`, `root:root 01777`) and any bind alias of it belong to the factory image: their mode keeps admin and application write access while protecting root-owned configuration entries, and Vigil never changes their owner or mode. Vigil sets the mounted root of every other slot (any device but `internal_device_path` of the storage C API) to `1000:1000`, mode `0777`; recording subdirectories are unaffected.

### Storage Status

Get the current status of all storage slots.

**Path:** `.../vigil/storage/status`

**Method:** `GET`

**Response Body:**

```json
{
    "lSlots": [
        {
            "sDevPath": "/dev/sda1", // String, the device path of the partition or volume
            "sMountPath": "/mnt/sda1", // String, the mount path, always set; below MOUNTED the mount may not exist
            "bRemovable": true, // Boolean, the device is removable
            "bInternal": false, // Boolean, the internal partition
            "sLabel": "MyDrive", // String or null, the volume label; null when unavailable or not yet formatted
            "sUUID": "550e8400-e29b-41d4-a716-446655440000", // String or null, the volume UUID, likewise
            "sType": "ext4", // String or null, the filesystem type, likewise
            "bSelected": true, // Boolean, the slot the configuration selects
            "bEnabled": true, // Boolean, the slot currently enabled
            "bDirty": false, // Boolean or null, the filesystem is marked dirty and may need fsck; null when not checked or unknown
            "bSyncing": false, // Boolean, indexing its files
            "bWriting": false, // Boolean, the slot's photo or video tasks are writing; a manual capture does not count
            "bRotating": false, // Boolean, quota rotation is running
            "eState": 8, // Integer, -1 ERROR, 1 NOT_FORMATTED_OR_FORMAT_UNSUPPORTED, 2 FORMATTING, 3 NOT_MOUNTED, 4 FILESYSTEM_CHECKING, 5 MOUNTED, 6 CONFIGURED, 7 INDEXING, 8 READY, 9 SATURATED
            "sState": "READY", // String, the name of eState
            "iStatsSizeBytes": 1000000000, // Integer, the partition size; accurate from MOUNTED on
            "iStatsFreeBytes": 500000000, // Integer, the free space; accurate from MOUNTED on
            "iQuotaMinimumRecommendBytes": 200000000, // Integer, the minimum recommended quota limit; a lower one may not work well
            "iQuotaPreservedBytes": 0, // Integer, the space preserved for the system; a limit above iStatsSizeBytes minus it may not be fully usable
            "iQuotaUsedBytes": 10000000, // Integer or null, the space used under the quota; null until the slot has started indexing, accurate from READY on
            "iQuotaLimitBytes": 100000000, // Integer, the quota limit, -1 for none; the default or inaccurate below CONFIGURED
            "bQuotaRotate": true // Boolean, quota rotation enabled; the default or inaccurate below CONFIGURED
        }
    ],
    "sDataDirPath": "DCIM" // String, the record data directory under each mount point
}
```

Notes:

- The status changes at runtime (devices, states, space, activity); poll it to follow the slots.
- `iQuotaUsedBytes` may exceed the sum of the recorded files slightly, by the filesystem's overhead.
- `iQuotaPreservedBytes` keeps the filesystem from filling completely, for system stability.
- `iQuotaMinimumRecommendBytes` is a guideline; a limit below it may perform poorly or not work at all.
- The quota limit and rotation take effect from `CONFIGURED` on and are stored per slot: one slot's settings do not affect another's.
- The quota limit is not always fully usable: when the free space is less than `iQuotaLimitBytes` minus `iQuotaUsedBytes`, the free space is the limit, and with rotation on the oldest files are deleted when the usable space runs out.
- Vigil mounts and unmounts the slots as needed and formats one only on a `FORMAT` request. One slot is enabled at a time: enabling one disables the one enabled before.
- Changing many files directly under the record data directory of an enabled slot can interfere with the storage system; remove files through Storage Control.
- A slot takes time to move between states; watch `eState` to see when it is ready.
- `bDirty`: the filesystem may need a check (fsck) before use, for example after a removal without an unmount or after write errors. It is not mounted automatically until repaired; enabling the slot attempts the repair (`fsck -p`).
- `SATURATED`: free space is critically low and new recordings stop. With rotation off the state holds until files are removed (`REMOVE_FILES_OR_DIRECTORIES` or `FREE_UP`), the quota limit is raised, rotation is turned on, or the configured file actions' critical watermark falls (one file action instead of two, or a smaller format); a recording accepted just before the state changed is canceled rather than left `PENDING` until then. With rotation on it waits for the rotation, which returns the slot to `READY`.

### Storage Control

Perform a sync or async storage action.

**Path:** `.../vigil/storage/control`

**Method:** `POST`

**Request Payload:**

```json
{
    "sTask": "ASYNC_STATUS", // String, optional, "SYNC" (the default), "ASYNC_SUBMIT", "ASYNC_STATUS", or "ASYNC_CANCEL"
    "sAction": "FREE_UP", // String, "FORMAT", "FREE_UP", "EJECT", "REMOVE_FILES_OR_DIRECTORIES", "CONFIG", "RELAY", "RELAY_STATUS", or "UNRELAY"
    "sSlotDevPath": "/dev/sda1", // String, the target slot's device path, required for every action
    "sTaskUID": "0-12345678901234567890", // String, optional, for ASYNC_STATUS and ASYNC_CANCEL; a non-empty value is ignored for now and an empty one matches nothing
    "dSlotConfig": { // Object, required for the sync CONFIG action
        "iQuotaLimitBytes": 1073741824, // Integer, the quota limit in bytes, -1 for none
        "bQuotaRotate": true // Boolean, quota rotation
    },
    "lFilesOrDirectoriesToRemove": [ // Array of strings, required for REMOVE_FILES_OR_DIRECTORIES with SYNC or ASYNC_SUBMIT
        "2025-10-16/event.mp4" // String, a path relative to the record data directory (sDataDirPath of Storage Status)
    ]
}
```

`FORMAT`, `FREE_UP`, `EJECT`, and `REMOVE_FILES_OR_DIRECTORIES` run sync or async; `CONFIG`, `RELAY`, `RELAY_STATUS`, and `UNRELAY` sync only.

**Response Body:**

The payload depends on `sTask`.

#### Sync (SYNC)

```json
{
    "dRelayStatus": { // Object, RELAY and RELAY_STATUS on success only
        "iRelayTimeoutRemain": 25, // Integer, the seconds left
        "iRelayTimeout": 300, // Integer, the timeout in seconds
        "sRelayDirectory": "..." // String, the relay directory name
    },
    "code": 0, "message": "" // see Error Handling
}
```

#### Async (ASYNC_SUBMIT, ASYNC_STATUS, ASYNC_CANCEL)

`ASYNC_SUBMIT` answers with the submitted task's object, normally still QUEUED with an empty `dResult`, plus `code` and `message`. `ASYNC_STATUS` answers with `lTasks`, the matching tasks' objects, each with `iQueuePosition`, and `ASYNC_CANCEL` with `lCancelledTasks`, the canceled tasks' objects without it; the array is omitted when no task matches, so the response may be just `{ "code": 0 }`.

```json
{
    "sUID": "0-12345678901234567890", // String, the task's unique ID
    "iCreatedTimestamp": 1776353445000, // Integer, Unix milliseconds of the task's creation
    "iUpdatedTimestamp": 1776353448000, // Integer, Unix milliseconds of the last update
    "sAction": "FREE_UP", // String, the action submitted
    "sDevPath": "/dev/sda1", // String, the target slot
    "sStatus": "RUNNING", // String, "QUEUED", "RUNNING", "SUCCEEDED", "FAILED", "CANCEL_REQUESTED", or "CANCELED"
    "bDone": false, // Boolean, SUCCEEDED, FAILED, or CANCELED
    "bCancelable": true, // Boolean, can still be canceled
    "iProgressPercent": 42, // Integer, 0 to 100, estimated
    "dResult": { // Object, the outcome so far; its fields vary by sAction, and these appear once a file operation runs
        "sFileOpsType": "FREE_UP", // String, "FREE_UP" or "REMOVE"
        "iRequestedCount": 10, // Integer
        "iProcessedCount": 4, // Integer
        "iSuccessCount": 4, // Integer
        "iFailedCount": 0, // Integer
        "iCode": 0, // Integer, optional, a non-zero system error code on failure
        "sMessage": "" // String, optional, the failure reason
    },
    "iQueuePosition": 0 // Integer or null, ASYNC_STATUS only: 0 running, 1 to N queued, null finished (its position at completion)
}
```

Runtime notes:

- `FORMAT` and `EJECT` can be canceled only while queued; formatting is allowed only in `NOT_FORMATTED_OR_FORMAT_UNSUPPORTED` or `NOT_MOUNTED`.
- `EJECT` answers after the slot is disabled, its filesystem synced, the unmount attempted, and the slot dropped from the status; a sync or unmount failure is logged, not reported. A device pulled and reinserted while the eject runs comes back as a new slot, listed after the answer.
- A `FREE_UP` or `REMOVE_FILES_OR_DIRECTORIES` still running on the slot holds its filesystem, so an eject only detaches the mount and the filesystem stays busy until that request returns; let it finish, or cancel it, before ejecting.
- `FREE_UP` and `REMOVE_FILES_OR_DIRECTORIES` can also be canceled while running.
- An accepted cancellation may still report `CANCEL_REQUESTED`; a later `ASYNC_STATUS` shows the final `CANCELED`.
- One async task runs at a time across all slots; the queue holds up to 6 pending tasks, and the history keeps the last 3 completed per `{sAction, sSlotDevPath}`. `ASYNC_SUBMIT` answers `EAGAIN` while the queue is full.
- `REMOVE_FILES_OR_DIRECTORIES` reports aggregates only: sync through `code` and `message`, async as the counts in `dResult`; no per-path result.

## Capture API

Manual capture, independent of the rule-based recording but for two things: the `ALWAYS_ON` rule opens no clip while a capture (or any other media operation) is active, and a host with a single video sink cannot run a video capture and a recording at once (see the capability limits).

### Capture Status

Get the current capture status.

**Path:** `.../vigil/capture/status`

**Method:** `GET`

**Response Body:**

```json
{
    "dLastCapture": { // Object or null, the capture in progress, else the last completed one
        "sID": "194e8b2c0000194e8b2c0001", // String, the capture's unique ID
        "sOutputDirectory": "/mnt/sdcard/DCIM/20260114", // String, the output directory
        "sFormat": "MP4", // String, "MP4", "JPG", or "RAW"
        "iVideoLengthSeconds": 60, // Integer, optional, video captures of finite length only
        "sStatus": "WRITING", // String, "PENDING", "WRITING", "COMPLETED", "FAILED", "INTERRUPTED", "CANCELED", or "UNKNOWN" (see the notes)
        "iTimestamp": 1736870400000, // Integer, Unix milliseconds
        "sFileName": "Capture 2026-01-14 14.30.00 123456.mp4" // String, the generated file name
    },
    "bReadyToStartNew": true, // Boolean, false while a capture is in progress
    "bStopRequested": false // Boolean, a stop is pending
}
```

Notes:

- `PENDING`: accepted and not yet running, also while media is paused.
- `COMPLETED`: an image was captured, or a video reached its length or its stop and ended on the next key frame or, if none arrived, when the key frame wait ran out.
- `FAILED`: a callback failed, the writer could not be built, the output filesystem had no headroom, or the finished file was missing or could not be renamed to its name (for example, a directory has that name); the capture leaves no file.
- `INTERRUPTED`: a pause, the service stopping, input loss, or free space below the critical watermark (checked every 60 seconds) ended a video after it started; the file is kept.
- `CANCELED`: nothing was recorded and no file exists, because media was paused or no key frame arrived within the key frame wait or before the capture was stopped.
- A status is final once `bReadyToStartNew` is true. Until then a capture that reads `COMPLETED` or `INTERRUPTED` can still turn `FAILED` if its file is missing or cannot be renamed to its name.

### Start Capture

Start a new capture session.

**Path:** `.../vigil/capture/start`

**Method:** `POST`

**Request Payload:**

```json
{
    "sOutput": "/mnt/sdcard/DCIM/20260114", // String, an absolute path under a storage mount point: a directory or a full file path
    "sFormat": "MP4", // String, "MP4" (video), "JPG" or "RAW" (image)
    "iVideoLengthSeconds": 60 // Integer, optional, the clip length in seconds; omit to record until stopped
}
```

**Response Body:**

```json
{
    "dCapture": { // Object, the started capture, in the shape of dLastCapture with sStatus "PENDING"
    },
    "code": 0, "message": "" // see Error Handling
}
```

Notes:

- `EBUSY`: a capture is already in progress; one runs at a time.
- The output path must be absolute and start with `/mnt` or the configured internal mount point. The check compares strings only, normalizes nothing, and looks for no mounted slot, so a path through `..`, or one that merely shares the prefix, passes and is written to, replacing a file of that name.
- A missing output directory is created (and stays if the capture then fails). The file is written under its name prefixed with a dot and renamed to `sFileName` when the capture ends; a stale dot-prefixed file is removed first.
- A capture is admitted against the same per-filesystem in-flight budget as automatic recording: when the output filesystem's free space, minus what admitted tasks may still write there, is at or below the capture's critical watermark, it fails before any callback (`FAILED`, no file); while it runs, its own estimate is in flight there and counts against automatic admission.
- Nothing runs while media is paused: the capture waits as `PENDING` and runs once media resumes.

### Stop Capture

Stop the current capture session.

**Path:** `.../vigil/capture/stop`

**Method:** `POST`

**Request Payload:** none (`{}` is accepted).

**Response Body:**

```json
{
    "code": 0, "message": "" // see Error Handling
}
```

Notes:

- `ENOENT`: no capture is in progress; `EALREADY`: a stop is already in progress.
- A stop ends a video capture as `COMPLETED` on the next key frame, the frames until then included, or when the key frame wait runs out without one; a clip that has not seen its first key frame is `CANCELED`. An image capture that has not run yet is still taken. The request is cleared with the capture.

## Files API

### Files Fetch

Fetch file contents through the relay.

**Path:** `.../vigil/relay/<relay_uuid>/<file_path>`

**Method:** `GET`

A client first requests a `RELAY` action through Storage Control, which creates a temporary relay directory (a symlink) named by a UUID, returned as `dRelayStatus.sRelayDirectory`, and then fetches files under it by their path relative to the record data directory, for example `.../vigil/relay/550e8400-e29b-41d4-a716-446655440000/2025-10-16/event.mp4`.

Notes:

- The relay directory times out 300 seconds after the last `RELAY` request; send `RELAY` again to keep it open. Fetching files does not refresh it.
- `UNRELAY` closes the relay explicitly.
- When the host provides `thumbnail_generate_cb`, a recorded video is published with a thumbnail: `/path/to/video.mp4` -> `/path/to/.thumb/video.mp4.thumb.jpg`.

## Error Handling

Every `POST` response carries `code`, 0 on success, else error_base (30000) plus abs(posix_errno), and may carry `message`; a `GET` response carries them only on failure. Request bodies are limited to 1 MiB; a larger body is rejected before parsing (the C API returns `EFAULT` with no JSON body).

```json
{
    "code": 30022, // Integer, 0 on success, else error_base + abs(posix_errno)
    "message": "..." // String, optional, present on failure
}
```
