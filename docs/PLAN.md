# Refactor Plan (DRAFT)

**Scope decision, 2026-10-19.** The `MIXED` rule (original requirement 1) is dropped. Rule selection, rule evaluation, and `record_rule_config.json` stay as they are today. This plan covers independent post-trigger actions (Photo, Video, Sound), standalone continuous recording, modest reporting improvements, and only the correctness fixes those features depend on. Where documentation and implementation disagree, the implemented behavior is kept and pinned by tests unless a row in section 2 says otherwise.

## 1. Goals and fixed decisions

1. Separate rule evaluation from post-trigger action admission and execution. Evaluation keeps producing one owner or nothing per tick, exactly as today; what changes is what happens after attribution.
2. Admit Photo (JPG/RAW), triggered Video (MP4), and Sound independently. Photo and Video have independent positive intervals; Sound has an enable switch only, no cooldown and no backlog.
3. Add independent continuous MP4. It ignores `bRuleEnabled` but **obeys the schedule**. While configured, it suppresses triggered Video without changing the saved triggered-video enable flag.
4. Keep direct discrete EVENT delivery as today: it bypasses the selected rule's filters and debounce, and on an EVENT tick the selected rule is not evaluated.
5. Keep `ALWAYS_ON` as a single-file-action rule. Never convert it to continuous MP4 or change JPG/RAW into video.
6. Keep RULE events simple: one owner's `type`, `id`, timestamp, UID, source, and delivery. No `trace`, no `triggers[]`, no action-result `actions[]`; `dActions` is a configuration dictionary only.
7. The only public C ABI changes are appended fields, both in Phase 4B: `sound_play_cb` in `vg_config_media_t` and `dispatch_timestamp` in `vg_rule_evt_t`.
8. Configuration migration is not this library's job. An external manager migrates `record_config.json` to the version-2 shape; the library reads and writes only that shape and rejects anything else without touching the file.

### Non-goals

- No `MIXED`, AND/OR expressions, rule priorities, arbitration windows, or three-state rule results.
- No change to `record_rule_config.json`, to rule replacement semantics, or to the first-match `InferenceRuleSet` behavior.
- No lossless GPIO/input/notification guarantee, durable notification bus, or generic action thread pool.
- No media-device implementation, playback resource parser, routing policy, or application event store inside Vigil. The boundary is the public callback contract, exercised locally with fakes.
- No unrelated storage/capture API redesign, dependency overhaul, or build-cache replacement.

### Decisions taken in review (2026-10-19)

- `source_delivery`, debounce zero, interval-zero rejection, the empty SED class filter, and the direct EVENT ID keep their implemented behavior (section 2).
- Schedule closure blocks new dispatch and new admission only; a task accepted while the gate was open may still start and finish, as today. Rule disable still cancels pending tasks and stops a running triggered video, as today (section 8.1).
- HTTP activation keeps today's `EBUSY` meaning: `code=0` when the RULE event was dispatched and, if any file action is enabled, at least one was accepted; `EBUSY` when every enabled file action was busy or unavailable (section 5.2).
- The single writer's 3-second failure cooldown and 10-consecutive-failure limit apply per channel; reaching the limit still marks the Slot failed, as today (section 7.2).
- Changing a channel's own settings still finalizes that channel's running task, as today; unrelated changes no longer do (section 7.1).
- A missing selected-rule file keeps today's `ALWAYS_ON` default (harmless while `bRuleEnabled` defaults to false); a corrupt existing file fails closed instead of leaving `ALWAYS_ON` live (section 4.3).
- The `sound_play_cb` ABI change moves to Phase 4B, so Phases 1 to 3 ship without a public layout change (section 11).
- Existing rule-info fields keep a defined meaning for single-file-action configurations; new fields carry per-channel state (section 6.2).
- No legacy configuration handling in the library: no `dWriterConfig` import, no mirrored GET, no mixed-shape or destructive-write detection. `iSchemaVersion` is required and must be 2; an external manager migrates old files (section 4.1).
- Every owner carries both an event time and a dispatch time (decided 2026-10-19, "carry both"): `timestamp` is the input's time for FRAME and direct EVENT owners and the observed time otherwise; `dispatch_timestamp` is when Vigil created the owner. Rule info reports both; `vg_rule_evt_t` gains the appended `dispatch_timestamp` in Phase 4B (section 6.1; done 2026-10-19).

## 2. Current module baseline and reading map

The current flow is a selected `BaseRule`, a single `SharedRuleEvent`, and one automatic Slot writer. `Context::tryCreateWriterEvent` couples file admission to rule reporting, and `AlwaysOnRule` uses the same path. The refactor must not simply add two threads to this shared state. Since Phase 4C-1 (2026-10-19) the shared event is one `Context::FileChannel` per capture type and the dispatch is `Context::dispatchRuleEvent`; since Phase 4C-4 (2026-10-19) the Slot runs one worker per channel.

| Area | Read first | Relevant current state |
| :--- | :--- | :--- |
| Rule implementations | `vigil/rules.hpp` | Evaluation stays as is. Fixed 2026-10-19 (Phase 3A): `TTYRule` leaked its `new char[]` read buffer on every rule replacement and its mismatch fallback lost overlapping prefixes (`AAB` was missed in `AAAB`); `GPIORule` checked that one of the two sync callbacks existed, not the one its signal mode uses. HTTP dispatches directly from the request thread and keeps doing so. |
| Inputs, clocks, admission | `vigil/context.hpp` | One shared file task until 2026-10-19 (Phase 4C-1), now one `FileChannel` per capture type holding a `FileTask` bound to its settings at acceptance; the identity-free reset was replaced by `claim`/`cancelPending`/`complete(expected)` on 2026-10-19 (Phase 2A); until 2026-10-19 (Phase 3A) rule disable returned before the clock update, which continuous recording needs, and inference input time overwrote the current wall time used for event and file timestamps; the clocks now advance on every tick, an input's timestamp is its owner's event time only, and the file task carries that event time. The direct EVENT owner built by the rule loop was stamped with the dispatch time, not the input time, until 2026-10-19 (section 2 table). The pause flag and the writer count were two unrelated atomics until 2026-10-19 (Phase 2B); `Context::MediaBarrier` now admits operations and pauses under one mutex. |
| Configuration | `vigil/configs.hpp`, `vigil/endpoints.hpp` | Before Phase 1C: single `dWriterConfig`, and `RecordConfig::sync` applied enablement before the settings revision check, so a conflict left a partial update. Since Phase 1C (2026-10-19): the version-2 `RecordConfig` is read whole, and `sync` publishes the writer settings under the revision check before the flag, so a conflict applies nothing to the runtime. Since Phase 4A (2026-10-19): what `sync` publishes is the `ActionSettings` snapshot with one revision per file channel, built before the commit lock. |
| Endpoint loop and startup | `vigil/endpoints.hpp` | Startup load failures used to leave the default `ALWAYS_ON` rule live; since Phase 1C an unusable file keeps rules disabled. `ConfigStore` used to write a temporary file and rename it without fsync, and reported a missing file as `EIO` because `is_regular_file` sets the error code for a not-found path; both fixed in Phase 1C (fsync before the rename; `ENOENT` for a missing file). |
| Identity/settings | `vigil/types.hpp` | `WriteEvent`, `EventOwner`, and `FileWriterSettings` are the building blocks to extend; `ActionSettings` joined them on 2026-10-19 (Phase 4A). `WaterMarks::from` read past its format table for an unknown format instead of using the fallback entry until 2026-10-19 (latent: every caller validates the format first; found by the Phase 1A tests). |
| Writers and publication | `vigil/writers.hpp`, `vigil/slot.hpp` | `VideoWriter` constructor failure used to leak `media_writers_active` (guarded since 2026-10-19, Phase 2A); any settings revision change interrupted the running writer until 2026-10-19 (Phase 4A), when each file channel got its own revision and a writer began watching only its own; since Phase 4C-3 (2026-10-19) a watermark-only change, such as a media refresh, moves no channel revision, and a running task continues against its own watermark instead of the Slot's readiness. Publication used to run in the destructor after the cooldown; since 2026-10-19 (Phase 2A) the writer thread releases the writer, and with it the finished file, before the cooldown; since Phase 4C-2 (2026-10-19) the loop is `FileChannelWorker` (`vigil/channel_worker.hpp`) and the Slot publishes after the writer released its media operation. Found 2026-10-19 while preparing Phase 2A: a writer constructor failure (for example JPG selected without `image_capture_cb`, or `video_file_create_cb` failing) is caught and the loop `continue`s without a sleep, a cooldown, or a consecutive-failure count, so the writer thread retried the same event in a hot loop with error logging; and `handle_write` completed whatever event was current rather than the one it ran, so a task finished after a disable/enable cycle could reset a newer event. Both fixed 2026-10-19 (Phase 2A): a constructor failure is a failed task with the same cooldown and failure count, and the thread completes only the event it claimed. Phase 4C-4 (2026-10-19): one worker per file channel, Photo and Video, over the same Slot; both publish under the publication lock, and `on_file_event_cb` may run from either. |
| Filesystem coordination | `vigil/index_manager.hpp`, `vigil/slot.hpp` | Until 2026-10-19 (Phase 2C) `EnsureDirectoryExists` removed a non-directory path before creating it, publication and removal were not coordinated, and a published file silently replaced an existing one of the same name. Directory creation is now idempotent and non-destructive, publication holds the Slot's publication lock against rotation, free-up, and explicit removal, and a taken name gets a numbered suffix. Automatic names are still timestamp plus extension. |
| Slot selection and lifetime | `vigil/storage.hpp`, `vigil/async_task.hpp` | Storage enables at most one Slot from configuration and netlink hotplug; Slot enable/disable starts and joins the manager thread and, since Phase 4C-4 (2026-10-19), the Photo and Video worker threads under the Slot mutex, with a failed start unwound; async storage tasks run on a separate worker. |
| C API and lifetime | `include/vigil_ctypes.h`, `include/vigil_capi.h`, `vigil/capi.cpp`, `vigil/vigil.hpp` | No playback callback or layout negotiation; `vg_media_scoped_suspend_write` waited only for video writers (automatic and manual), not image capture or thumbnail callbacks, until 2026-10-19 (Phase 2B); `vg_service_deinit` destroyed the runtime while holding the instance lock, so a callback that reentered the C API during shutdown deadlocked, until 2026-10-19 (Phase 2B). Phase 4B (2026-10-19) appended `sound_play_cb` and `dispatch_timestamp` and bumped both version strings to `2026.10.19`; the layout note sits on `vg_config_media_t`. |
| Input schemas | `protocol/inference.proto`, `3rdparty/acousticslab_proto/` | Preserve FRAME/EVENT interpretation and SED input framing; do not invent another ingress protocol. |
| Build and examples | `CMakeLists.txt`, `README.txt`, `example/` | C++20 library target `vigil`; examples are not a deterministic regression suite. |
| Existing documentation | `docs/minimal-http-api.md`, `docs/architecture.dot` | Describe the current API and architecture, with the discrepancies listed below. |

Current defaults worth preserving unless explicitly changed here: 10 ms target rule-loop period, 50 ms Slot-writer and manual-capture probe period, inference queue capacity 60 with oldest-entry dropping, 128 x 128 ROI masks, 60-second target video clips, approximately 10 seconds of keyframe-aware media buffering, a 5000 ms writer interval, and the single writer's 3-second failure cooldown with a stop after 10 consecutive failures.

### Known documentation/implementation discrepancies

These are separate compatibility decisions, not permission to make incidental behavior changes. The default is to keep the implemented behavior and correct the documentation.

| Topic | Current discrepancy | Decision |
| :--- | :--- | :--- |
| Inference debounce | API prose says consecutive frames; implementation increments/decrements a saturating counter. | Keep the counter and correct the explanation; absent or unrelated input is not negative input. |
| Debounce zero | Zero makes every frame from a matching source activate, including negative frames. `iDebounceTimes` is parsed as `size_t` and narrowed to `int`, so negative or oversized values are not rejected. | **Kept (2026-10-19).** Document the zero semantics as is; reject only negative and oversized values at parse time. |
| Recording interval zero | Documentation permits it; the parser rejects `<= 0`. | **Kept (2026-10-19).** Version-2 Photo/Video intervals must be positive like `dWriterConfig.iIntervalMs`; correct the API prose, including the unreachable `ALWAYS_ON` zero-interval sentence. |
| Empty SED class filter | Documentation says match any label; implementation matches none. | Keep and pin with a test; correct the documentation. |
| Direct EVENT ID | Documentation describes source-as-condition-ID; implementation uses an empty ID and a separate source ID. | Keep the empty condition ID and separate source; correct the documentation. |
| Rule info | Existing last-event prose and current-task selection differ; the response also emits `iOwnerUID`, `iUID`, `sSourceID`, and `sSourceDelivery`, which the API document omits. | Keep the existing fields and document them; add the independent last-dispatched owner and per-channel action status from section 6.2. |
| TTY matching and diagram | The architecture diagram describes a Boyer-Moore matcher, names `tryUpdateRuleEvent()`, and omits the SED and ALWAYS_ON rules; the code is a streaming prefix matcher whose mismatch fallback loses overlapping prefixes. | Fix the matcher defect and the buffer leak with tests, then update the diagram with the missing rules and the real `tryCreateWriterEvent` name. |
| Media suspension | Header wording was broader than the video-only activity accounting until Phase 2B (2026-10-19). | The barrier covers image capture and thumbnail generation since Phase 2B (section 8.3) and Sound playback since Phase 4B (2026-10-19). |
| Channel busy | Section 6.2 said `bReadyForNewEvent` is true when no channel holds a pending or running task; the writer holds the claimed event through the interval (and the failure cooldown) before completing it, and ALWAYS_ON paces on that hold. | **Decided (2026-10-19): a channel holds a task from acceptance to completion, which follows the hold.** One predicate for ALWAYS_ON, `bReadyForNewEvent`, and admission (Phase 4C); `FileChannel::holdsTask` since Phase 4C-1 (2026-10-19). |
| Source delivery without a source | Every non-inference owner (SED, Timer, GPIO, TTY, HTTP, ALWAYS_ON) is stamped `source_delivery=EVENT` even with an empty `source_id`, while the header says empty means unknown. | **Kept (2026-10-19).** Pin it with a test and correct the header comment; the emitted value does not change. |
| Direct EVENT timestamp | Section 6.1 says FRAME/EVENT owners keep the input event time. Inference-member owners did; the direct EVENT owner was stamped with the dispatch wall time, and the input time only reached the shared clock (and from there the writer event). | **Decided (2026-10-19): carry both.** Every owner has `timestamp` (event time: the input's time for FRAME and direct EVENT owners, the observed time otherwise) and `dispatch_timestamp` (when Vigil created it). Done ahead of Phase 3A: the direct EVENT owner carries the input time, rule info adds `iDispatchTimestamp`, and `vg_rule_evt_t` gained the appended field in Phase 4B (2026-10-19). The shared-clock separation itself stays in Phase 3A. |

Before Phase 1A the source had no first-party `tests/` directory or CTest registration, and no commit in the history had ever added one; a fresh configure in the dev container on 2026-10-19 reported `ctest -N` as `Total Tests: 0`. Phase 1A (2026-10-19) added `tests/` behind `VIGIL_BUILD_TESTS` (section 12.1). `vigil_example` is a demonstration; `vigil_index_manager_test` is an interactive filesystem monitor that needs fanotify privileges; `vigil_mask_test` prints examples without assertions. `build/` is git-ignored and absent from both this checkout and the dev-container mirror; generated binaries or CTest registrations found in any stale build directory are not reproducible evidence and must not be reused as the test source of truth.

## 3. Target architecture and responsibilities

| Layer | Responsibility | Must not own |
| :--- | :--- | :--- |
| Configuration/endpoints | Parse, validate, prepare, and commit immutable settings; expose capabilities/status; publish gate transitions. | Blocking playback, worker joins under configuration locks. |
| Rule evaluation | Evaluate the schedule gate and the selected rule exactly as today; produce at most one attributed owner per tick. | File availability, action cooldowns, storage I/O, playback. |
| Action dispatch/execution | Independently admit immutable Photo/Video/Sound tasks; own worker state, settings, budgets, finalization, and cancellation. | Changing a captured owner's identity. |
| Public C API/lifetime | Validate inputs, maintain runtime lifetime, expose borrowed event data, invoke bounded media callbacks. | Device-specific playback or a guarantee that arbitrary callbacks can be canceled. |

The normal path is evaluation, attribution, release of rule/configuration locks, independent channel admission, and the RULE callback. FILE callbacks follow successful publication by the file workers. Continuous video starts internally in the one Video worker and does not fabricate RULE events.

`SED` is a sound-detection **input rule**. `dSound` and `sound_play_cb` are sound-playback **output actions**. They are independent features.

## 4. Configuration and commit contract

### 4.1 Record action configuration

`rule/config`, persisted as `record_config.json`, gains this canonical version-2 shape:

```json
{
    "iSchemaVersion": 2,
    "bRuleEnabled": true,
    "bContinuousRecordEnabled": false,
    "dActions": {
        "dPhoto": {
            "bEnabled": true,
            "sFormat": "JPG",
            "iIntervalMs": 5000
        },
        "dVideo": {
            "bEnabled": true,
            "iIntervalMs": 5000
        },
        "dSound": {
            "bEnabled": true
        }
    }
}
```

Rules for parsing and canonicalization:

- Only this schema is read, and it is read whole: `iSchemaVersion` must be the integer 2 and every field is required, in the shape GET returns. Any other version, the old `dWriterConfig` shape, or an incomplete document is rejected with the invalid-argument error and the file is left untouched; migrating old files is the external manager's job, and the library imports nothing from them. Unknown fields are ignored and dropped, as today.
- First-start defaults: `bRuleEnabled=false`, `bContinuousRecordEnabled=false`, Photo disabled with `sFormat="JPG"` and `iIntervalMs=5000`, Video disabled with 5000 ms, Sound disabled. A first start without configuration must not enable automatic behavior.
- Fields of the wrong type are errors, not defaults. Photo is JPG or RAW; Video is always MP4. Sound has no interval, file path, playback queue, or per-trigger payload.
- Photo/Video intervals are positive integers, the same rule as today's `iIntervalMs`: zero, fractions, negatives, and values outside a signed 64-bit integer are rejected. There is no upper bound; the manager is trusted to send intervals that fit the clock arithmetic (stated in `RecordConfig`).
- The earliest next start in a channel is `max(actual_start + interval, finalization_completion)`, subject to the current gates. An interval shorter than the write duration adds no delay; a channel never runs two automatic tasks at once.
- `bContinuousRecordEnabled=true` disables triggered Video admission even when continuous recording is outside schedule, paused, or unable to access storage. Preserve the saved `dVideo.bEnabled` value.

GET and persistence return the canonical version-2 JSON. Until Phase 4D, a payload that enables more than one file action, Sound, or continuous recording is rejected rather than accepted as a no-op; so is `bRuleEnabled=true` with no enabled file action, because the single-writer runtime cannot represent a rule-only configuration yet. `bRuleEnabled=false` with every action disabled is accepted (it is the first-start state); its snapshot has no file channel, and the Slot keeps the first-start headroom, the watermarks of one default clip (since Phase 4A, 2026-10-19; until then the last file action's writer settings stayed in place).

### 4.2 Selected rule and schedule configuration (unchanged)

`rule/record/config`, persisted as `record_rule_config.json`, keeps its current single-selection shape, child blocks, validation, and error convention; it gains no version field. `schedule_rule_config.json` remains the separate gate with its established local-time semantics; a disabled schedule gate is open. Rule replacement keeps today's destroy-then-construct order and best-effort rollback.

### 4.3 Commits and startup

1. Parse and validate the whole document before taking any lock; the limits of the current phase are part of validation. Phase 4A adds the immutable `ActionSettings` snapshot here. Do not call media callbacks under the commit lock. Done 2026-10-19 (Phase 4A): `RuleEndpoint::updateRecordConfig` builds the snapshot right after parsing, so the host's `max_bytes_estimate_cb` runs outside the commit lock.
2. Configuration commits are serialized by the endpoint's configuration mutex. A media-parameter refresh may still run concurrently: `RecordConfig::sync` publishes the snapshot under the global revision check and only then the rule flag, so a conflict applies nothing to the runtime and the POST answers `EAGAIN` for the client to retry. The stored document is already the new one by then, as today. Since the review fix of 2026-10-19 the revision is captured before the snapshot is prepared (the host's estimate runs outside the lock) and re-checked under the lock before storing, so a refresh or another commit that lands first answers `EAGAIN` with nothing stored; only a refresh between the store and the publish leaves the new file with the old runtime.
3. Store first: write and fsync the temporary file, then rename it; a failure before the rename leaves the old file and the old runtime. Publish to the runtime only after the rename.
4. The two configuration POSTs are not a multi-file transaction. They share serialized combined-state validation, especially the `ALWAYS_ON` invariant below.
5. At startup, missing files keep today's defaults (`bRuleEnabled=false`, every action disabled, continuous off, `ALWAYS_ON` selected, schedule gate open). A file that exists but cannot be loaded or applied is left untouched and keeps rules disabled until a valid `rule/config` is posted; this one lever keeps a corrupt selected-rule file from leaving the default `ALWAYS_ON` rule live and a bad schedule from becoming an always-open gate. GET reports the defaults for the unusable file and `bRuleEnabled=false`.

### 4.4 ALWAYS_ON invariant

`ALWAYS_ON` paces **one enabled file channel**, with Sound disabled and continuous disabled. Its interval, MP4/JPG/RAW format, rule-enable gate, schedule admission, and RULE/FILE UID association remain meaningful. Do not turn it into a multi-action scheduler or remove gaps caused by its interval.

Both `rule/config` and `rule/record/config`, plus startup, validate this invariant against the other configuration's current revision in the shared commit protocol. Concurrent or differently ordered POSTs must not bypass it. To configure several actions, select a supporting rule such as TIMER first; to switch back, simplify the actions first. Make `ALWAYS_ON` work on the new channel machinery **before** exposing the multi-action API, not as a final cleanup. Status 2026-10-19 (Phase 4C-1): `AlwaysOnRule` paces on `FileChannels::holdsTask`, either channel's hold (`always_on_waits_for_the_current_event_and_for_media`, `tests/test_context.cpp`); the invariant check itself comes with Phase 4D.

## 5. Rule evaluation (preserved)

### 5.1 Tick order

1. Update steady time, current wall time, and schedule state before considering `bRuleEnabled`. Continuous recording needs the current time even when rule evaluation is disabled.
2. Obtain at most one queued inference input with the existing bounded-queue policy. Keep FRAME subscription behavior and direct EVENT intake without inference-rule subscribers.
3. When rule enablement and the schedule allow: a direct EVENT becomes the owner and the selected rule is not evaluated on that tick; otherwise evaluate the selected rule once, exactly as today, including first-match `InferenceRuleSet` selection.
4. Finish attribution, release rule/configuration locks, recheck the gates, then call `dispatchRuleEvent(owner)` (section 7.1).

Keep evaluation free of storage operations and playback. The 10 ms loop period remains a measured objective, not a guarantee.

### 5.2 Leaf behavior

- **InferenceRule / InferenceRuleSet:** unchanged. The saturating counter, source/confidence/class/ROI filters, and first-match selection stay as they are. `resetInference` / `vg_inference_queue_reset` clear queued input only. Missing frames, unrelated sources, and direct EVENT input do not touch the counter. Debounce zero keeps its current meaning (section 2).
- **SED:** unchanged, including the consecutive-time-window algorithm, the 64 KiB frame bound, reconnect backoff, and the latched activation consumed on evaluation. Document silence, ignored packets, disconnect, and gate-closure behavior as observed; do not add an idle timeout to the first-header-byte wait.
- **TTY:** fix the two defects only: free the read buffer (RAII) and make the mismatch fallback find overlapping prefixes such as `AAB` in `AAAB`. Other TTY behavior is unchanged; do not add a partial-command timeout.
- **GPIO:** validate the sync callback required by the selected signal mode at construction. Level and edge semantics are unchanged: sustained matching levels retrigger, and edges are observed on evaluation, not queued.
- **Timer, ALWAYS_ON, Schedule:** unchanged.
- **HTTP:** activation stays on the request thread and calls `dispatchRuleEvent` directly; the RULE callback fires on every dispatch, as today. The response keeps today's meaning: `code=0` when the event was dispatched and, if any file action is enabled, at least one was accepted; `EBUSY` when every enabled file action was busy or unavailable. For a single-file-action configuration this is exactly today's behavior; it never means played or written. Bound the request body before JSON parsing. The storage-readiness precheck applies only when a file action is enabled, so rule-only and Sound-only activation work without storage.

Status 2026-10-19 (Phase 3A): the clocks tick before the enable check (the schedule state does not yet: it is still evaluated after the check, and Phase 4C publishes it before the check for admission), the clock separation, the TTY and GPIO fixes, the request-body bound (`kHTTPAPIRequestMaxBytes`, 1 MiB, checked before parsing for every POST), and the file-action-only storage precheck are in place. HTTP activation already dispatched through the rule loop's `tryCreateWriterEvent`, renamed `dispatchRuleEvent` in Phase 4C-1 (2026-10-19); since the review fix of 2026-10-19 it does so with no rule lock held, so a RULE callback may post a configuration (`http_activation_runs_the_rule_callback_outside_the_rule_lock`). Pinned by `tests/test_peripheral_rules.cpp` (GPIO callback validation, levels and edges, the TTY matcher), `tests/test_context.cpp` (clocks), and `tests/test_capi.cpp` (body bound).

## 6. Event model and reporting contract

### 6.1 Attribution

`vg_rule_evt_t` keeps its current layout until Phase 4B appends `dispatch_timestamp` (done 2026-10-19). The only change in event content is filling the empty `id` and `source_id` of GPIO and TTY owners (done 2026-10-19, Phase 3A; `tests/test_peripheral_rules.cpp`); everything else is current behavior, pinned by tests.

| Origin | `type` | `id` | `source_id` | `source_delivery` |
| :--- | :--- | :--- | :--- | :--- |
| Inference member | `INFERENCE_SET` | Configured member `sID` | Input source ID | FRAME |
| Direct EVENT | `UNKNOWN` | Empty | Input source ID | EVENT |
| SED | `SED` | Configured model/head `sID` | Empty | EVENT |
| GPIO | `GPIO` | Configured signal, such as `RISING` (new; empty today) | Resolved pin name (new; empty today) | EVENT |
| TTY | `TTY` | Configured command (new; empty today) | TTY name (new; empty today) | EVENT |
| Timer | `TIMER` | Empty | Empty | EVENT |
| HTTP | `HTTP` | Empty | Empty | EVENT |
| ALWAYS_ON | `ALWAYS_ON` | Empty | Empty | EVENT |

`source_delivery` is kept as is (decided 2026-10-19): FRAME for inference frame owners and EVENT for every other owner, including owners with an empty `source_id`. The header comment's "empty when unknown" wording was corrected on 2026-10-19; the emitted value did not change. SCHEDULE remains a gate, never an emitted owner.

Capture `uid`, both timestamps, condition ID, and source once; published owners are read-only. Every owner carries two times (decided 2026-10-19): `timestamp`, the event time, is the input's timestamp for FRAME and direct EVENT owners and the observed time for every other owner; `dispatch_timestamp` is when Vigil created the owner, and equals the event time for owners without an input. The C callback keeps `timestamp` and gained `dispatch_timestamp` in Phase 4B (2026-10-19; `tests/test_context.cpp`); rule info reports both as `iTimestamp` and `iDispatchTimestamp`. The current wall time stays independent of inference timestamps (since 2026-10-19, Phase 3A). FILE timestamp is the actual publication time. Preserve exact 64-bit UIDs across module-owned JSON output, including values above `2^53`; do not route integers through floating point. UID uniqueness is process-local, not a cross-restart guarantee.

### 6.2 RULE and FILE behavior

- Each permitted dispatch updates `last_rule_owner` and attempts `on_rule_event_cb`, even if all actions are disabled, busy, paused, or unavailable.
- `writer_created = photo_accepted || video_accepted`. It means at least one file task was accepted, not that a file was created or completed, and not whether Sound played.
- File tasks carry the admitted owner's UID and settings. Photo and Video may both publish FILE callbacks for one RULE; zero successful files is also valid.
- Continuous files use `event_uid=0` with their own unique file identity; no synthetic RULE with UID zero is created. Never use the event UID alone as a file identifier.
- There is no expected-file-count or all-actions-completed signal, and no event `actions[]` array.
- Callbacks are best-effort notifications, not a durable ordered stream. FILE may be observed before RULE because of concurrent execution. Do not hold internal locks across notification callbacks.
- Rule info adds the last **dispatched** owner, independent of file tasks, plus Photo/Video readiness and cooldown state and Sound busy/unavailable state. Owner objects carry `iTimestamp` (event time) and `iDispatchTimestamp` (since 2026-10-19). The existing fields keep a defined meaning: `bReadyForNewEvent` is true when no file channel holds a task, where a channel holds a task from acceptance until completion, which follows the interval hold and the failure cooldown (decided 2026-10-19: today's behavior, which ALWAYS_ON pacing depends on); `dLastRuleEvent` and `dLastRuleEventOwner` describe the most recently accepted file task across both channels, by acceptance sequence: the current task accepted last if any channel holds one, else the last completed one (decided 2026-10-19). For a single-file-action configuration this is identical to today.

Status 2026-10-19 (Phase 4C-1): `writer_created` is `photo_accepted || video_accepted` (`dispatch_accepts_into_every_enabled_channel_and_reports_either_acceptance`, `tests/test_context.cpp`; the two-channel snapshot is built directly, since the parser enables one channel until Phase 4D). `Context::last_dispatched_owner` records every dispatch independently of acceptance; it is not reported yet. Rule info computes `bReadyForNewEvent`, `dLastRuleEvent`, and `dLastRuleEventOwner` through `FileChannels::latest`, by acceptance sequence across both channels (`rule_info_reports_the_channel_busy_through_the_hold`, `tests/test_rule_endpoint.cpp`). The last dispatched owner, the per-channel readiness and cooldown state, and the Sound state are exposed with the Phase 4D API.

## 7. Action execution

### 7.1 Settings and independent admission

Use an immutable `ActionSettings` snapshot: rule/continuous enablement, action enablement, Photo settings, triggered Video settings, continuous Video settings (MP4, no extra interval), and effective watermarks. Keep revisions and runtime state separate from persisted fields.

The global revision detects configuration-commit conflicts; it must not interrupt every writer on every change. Channel settings invalidate only their own pending tasks, and changing a channel's own settings still finalizes that channel's running task, as today. A Photo interval or Sound switch update must not interrupt a running Video task. Media-parameter refresh updates triggered and continuous Video settings coherently. Readers (the rule thread, the HTTP thread, the workers) load the snapshot without taking the commit lock.

Status 2026-10-19 (Phase 4A): `ActionSettings` (`vigil/types.hpp`) holds the Sound and continuous switches, the Photo and triggered Video `FileWriterSettings` (null when disabled), and the effective watermarks (the enabled file channel's, or one default clip's when none is enabled). `Context::SharedActionSettings` publishes it under the global revision and keeps one revision per file channel, bumped only when that channel's `FileWriterSettings` differ (watermarks included until Phase 4C-3, since 2026-10-19 the recording settings only; section 9); `SlotAdaptedWriter` watches its own channel's revision. A media refresh re-estimates every channel into one new snapshot. Rule enablement stays the `record_rule_enabled` gate, published after the snapshot. The single-writer behavior is unchanged: the Slot's writer thread serves the one enabled file channel through `ActionSettings::fileChannel`, and rules cannot stay enabled without one. Continuous Video settings join the snapshot in Phase 5A; independent admission comes with Phase 4C. Pinned by `tests/test_record_config.cpp` (channel settings and effective watermarks, per-channel revisions, the refused stale revision, the refresh).

Decided 2026-10-19 (review before Phase 4C), the three definitions the per-channel state rests on: a channel is busy from acceptance until completion, which follows the interval hold and the failure cooldown (section 2, "Channel busy"); the schedule gate is published into `Context` by the rule loop before the enable check and read inside the admission section (section 8.1); and admission readiness is not continuation (section 9). Lock order: the dispatcher loads the snapshot first, then takes a channel lock and re-verifies the channel revision inside it; a settings publish never cancels tasks under the settings mutex, the caller cancels after `update` returns.

Status 2026-10-19 (Phase 4C-1): `dispatchRuleEvent` loads the snapshot once and calls `FileChannel::tryAccept` for each channel it enables, which re-verifies the channel revision under the channel lock, so a task is never bound to settings that a concurrent commit replaced; `RecordConfig::sync` cancels the pending task of each channel whose revision moved, after `update` returns (`sync_cancels_the_pending_task_of_a_channel_whose_settings_changed`, `tests/test_record_config.cpp`). This is the one behavior change of the step, decided above: before it a pending event survived a change of its own channel's settings and was written with the new ones; now it is canceled and the next tick dispatches again. A claimed task is left to its worker, which stops on the revision change as before.

After attribution and lock release, `dispatchRuleEvent(owner)` observes one consistent configuration/gate state and independently attempts the three channels. Photo/Video require a usable Slot, media availability, and an idle, cooldown-ready channel; Video additionally requires continuous to be unconfigured. Sound requires no storage. A later failure in one channel never rolls back another.

Admission and gate closure share one linearization point, so a result from an old tick cannot reopen a closed channel. `dispatchRuleEvent` may run concurrently on the rule thread and the HTTP request thread and must be safe for that. Workers recheck admission and resource identity before starting. Each channel holds at most one accepted pending-or-running automatic task; there is no queue.

### 7.2 File task lifecycle and parallel workers

Use identity-aware operations such as `tryAccept`, `claim`, `complete(expected_task)`, and `cancelPending`. An accepted task binds owner, immutable settings, and Slot identity. Canceling a pending task differs from asking a running task to stop. Completion of old task A must never reset newer task B. Decided 2026-10-19: the per-channel fields (kind, settings, Slot identity, acceptance sequence, hold, stop request) live on a task object that wraps `WriteEvent`, which stays shared with manual capture and `CacheFileInfo`, and `requestStop` on a running task is distinct from `cancelPending`.

Status 2026-10-19 (Phase 4C-1): `FileTask` (`vigil/types.hpp`) wraps the `WriteEvent` with the kind, the owner, the settings and channel revision bound at acceptance, and the acceptance sequence; `Context::FileChannel` implements `tryAccept`, `claim`, `cancelPending`, and `complete(expected)` per channel, and `Context::FileChannels` holds the Photo and Video channels with the cross-channel predicates (`channels_hold_tasks_independently_and_a_stale_task_never_touches_a_newer_one`, `tests/test_context.cpp`). The Slot's one worker claims from the enabled channel, creates the writer from the task's settings and revision, and completes the task after the hold, as before. The Slot identity joins the task in Phase 4C-5, where admission at dispatch knows the Slot, and `requestStop` with its first caller, the continuous-mode handover of Phase 5A.

Status 2026-10-19 (Phase 4C-2): the per-task loop is `FileChannelWorker<Adapter>` (`vigil/channel_worker.hpp`), over an adapter with the Slot's readiness, headroom probes, writer factory, publication, failure mark, and stop request; the Slot runs one over itself. A task runs as claim, write, `takePublication` (the thumbnail is generated while the writer holds its media operation), release of the writer and its operation, publication (rename, accounting, FILE callback) with no operation held, then the hold and the completion. The cooldown and the consecutive failure limit are constructor parameters with the production defaults, now in `vigil/constants.hpp`. Pinned by `tests/test_channel_worker.cpp` over a fake Slot and `tests/support/fake_media.hpp`: the sequence with the publication before the hold and outside the operation, the cooldown and the limit with injected constants, Canceled and Interrupted without a hold, the stop ending a hold, the stale-task cancellation of the thread body, and a stale task A never touching a newer B through the loop.

Status 2026-10-19 (Phase 4C-4): the Slot runs one `FileChannelWorker` per file channel, bound to its `CaptureType` at construction: each claims from its own channel (`ActionSettings::channel(type)`), retires only its own channel's stale task when it starts, counts its own failures, and admits through `Slot::tryAdmit(admission, task_bytes)` against the aggregate mark with its own bytes reserved. `Slot::enable` starts the manager thread and both workers and unwinds the threads already running when one cannot be started; `disable` joins the workers, then the manager, before the index is released; either worker reaching its failure limit still marks the Slot failed, which takes the other channel down with it (the retained limitation above). Pinned by `tests/test_channel_worker.cpp`: two workers over one fake Slot, neither claiming from the other's channel, both reservations in flight at once and each released with its own publication, a failure limit reached on the Photo channel alone, each thread body retiring only its own stale task, and the Photo worker publishing while the Video worker waits for the key frame that closes its clip (matrix A1); and by `tests/test_capi.cpp`, which selects the internal Slot so the runtime starts and stops with the manager and both workers under TSan. The partial start has no injection seam and is covered by reading.

The normal sequence is:

`claim -> admission recheck -> create/write -> finish -> publish -> terminal result -> release -> cooldown`

- Successful completion includes sink finish and non-overwriting final publication. Failed create/write/finish/rename must not emit a success FILE. Define how a usable interrupted clip is retained and reported without labeling it an ordinary completed task.
- Finish the sink explicitly on every path (a finish failure fails the task), then release the writer, which publishes the file and balances accounting, before waiting for the channel interval. Never keep a writer alive through the cooldown; destructors only clean up what an exception left behind.
- Constructor failures, runtime errors, and finalization failures all retire the expected task, balance resources and budgets, and enter the failure backoff. Keep today's constants per channel: a 3-second cooldown after a failed task and a stop after 10 consecutive failures, which marks the Slot failed as it does today. A persistently failing Photo callback therefore still stops automatic recording on that Slot; document this as a retained limitation.
- Create one Photo worker and one Video worker per enabled Slot only after the single-writer lifecycle is safe. Each is serialized within its own channel; they progress independently when callbacks and resources permit.
- Photo calls JPG/RAW capture and publishes without consuming `slot_media_buffer`. It still participates in media pause and resource admission.
- Only the automatic Video worker consumes `slot_media_buffer`; manual capture keeps its separate buffer and lifecycle. Two threads do not imply unlimited sink capacity or hardware-level parallelism.
- When automatic video and manual video coexist, callback sink-capacity errors are real failures. Every nonzero status, including positive EBUSY, is an error; finish/error paths release acquired resources under the callback contract.

### 7.3 Sound executor and callback

Append the optional callback `vg_sta_e (*sound_play_cb)(void *user_data)` to `vg_config_media_t`. The module default is null/unavailable. A configured action without a callback must not be reported as accepted playback; expose the unavailable condition or a clear diagnostic. Zero means success and nonzero means failure, following the established callback status convention.

Implement one lightweight `SoundActionExecutor`, owned within the runtime/Context:

- State is `Idle / Pending / Playing / Stopping` under one mutex and condition variable.
- `tryTrigger` checks settings, gate/pause/stop, callback availability, and Idle in a short critical section. Only Idle becomes Pending. Pending/Playing retriggers are rejected, not queued.
- The worker claims Pending, enters Playing, releases the lock, and invokes the callback. All success and error paths restore the appropriate state and notify waiters. Errors affect Sound only and do not replay the trigger.
- Disabling Sound, closing the trigger gate, or stopping cancels Pending. Playing finishes through the callback's bounded completion contract; only then may the worker exit or accept another trigger.
- Explicit `stopAndJoin` precedes destruction. Never join while holding the executor mutex, detach the worker, or forcibly terminate it. Stop and acceptance are linearized.

The callback contract requires **bounded total completion**, including its cleanup, and valid callback data until the worker joins. No cancellation token is introduced. A timer or stop flag inside Vigil cannot safely interrupt an arbitrary blocking C function; if the callback cannot meet this contract, keep the capability unavailable rather than claiming bounded shutdown. The executor serializes only its own requests.

A SED rule combined with the Sound action can retrigger itself through the microphone. Vigil does not suppress this; document it as a capability limit and leave any guard time to the host's playback implementation.

Status 2026-10-19 (Phase 4B): `Context::SoundActionExecutor` (`vigil/context.hpp`) implements the four states under one mutex and condition variable. `tryTrigger` accepts from Idle only while the callback exists, media is not paused, and the executor is not stopping; the worker claims a Pending trigger, enters the media barrier (a refused admission cancels the trigger), calls `sound_play_cb` outside every lock, logs a nonzero status, and returns to Idle; nothing is queued or retried. `Context::tick` (rules disabled), `RecordConfig::sync` (Sound disabled), and `stopAndJoin` cancel a Pending trigger, and `Vigil::~Vigil` stops the executor right after the rule producer, waiting for a Playing callback. Without a callback no worker is started and Sound is unavailable. The dispatch path attempts Sound when the snapshot enables it, which no configuration can do before Phase 4D; `writer_created` stays file acceptance only. Pinned by `tests/test_sound_executor.cpp` (unavailable, single play with Pending and Playing retriggers rejected, failure without retry, cancellation by rule disable, Sound disable, pause, and stop, the waiting pause and the stop returning only after the callback, and dispatch). Review fix 2026-10-19: the worker rechecks Sound and rules after claiming, so a commit that closed a gate between the dispatcher's snapshot load and the trigger cancels it (`a_claimed_trigger_is_canceled_when_a_gate_closed_since_the_trigger`).

## 8. Gates, continuous video, and lifetime safety

### 8.1 Gate matrix

| Condition | Selected rule and direct EVENT | New triggered actions | Independent continuous Video |
| :--- | :--- | :--- | :--- |
| `bRuleEnabled=false` | No new dispatch. | Reject and cancel pending. | Unaffected by this flag. |
| Schedule closed | No new dispatch. | No new admission; a task accepted while open may still start and finish (today's admission-only policy). | No new clip; request current clip finalization. |
| Continuous configured | Evaluate/report normally. | Photo/Sound normal; triggered Video suppressed. | Run when schedule/storage/media allow. |
| Storage unavailable or insufficient | Evaluate/report normally. | Reject file actions only; Sound remains eligible. | No new clip; finalize/stop as required by failure severity. |
| Media paused | Rule reporting may continue. | Photo/Video/Sound admission closed; a running triggered video is interrupted and finalized, as today. | Stop/finalize without retaining old device resources across reconfiguration. |
| Service stopping | No new dispatch. | Close all admission and cancel pending. | Stop/finalize and join. |

Rule disable cancels pending triggered actions and asks a running triggered video to stop, as today; schedule closure only stops new dispatch and admission. Decided 2026-10-19: the schedule state is published into `Context` by the rule loop on every tick before the enable check (Phase 4C-5) and read inside the admission section, so dispatch from the rule thread and the HTTP thread sees one gate state; a worker's recheck before starting covers identity (Slot, channel revision), `bRuleEnabled`, and media pause, never the schedule, so a task accepted while the schedule was open still starts and finishes. One deliberate change: today a pending event waits indefinitely for a ready Slot and blocks every later event meanwhile, whereas independent admission rejects the file actions at dispatch and later events proceed. Leaf rule state keeps today's behavior: inference counters survive, the inference queue is cleared while rules are disabled, Timer does not replay missed intervals, and GPIO level reads the current value on reopening. Configuration replacement rebuilds rule instance state as today.

An already-started triggered video keeps the existing admission-only schedule behavior and may finish after the schedule closes. Disabling the rule system requests that triggered video stop. Already-started Photo/Sound callbacks finish within their bounded contract. Continuous video is different: schedule closure is a running-stop condition.

### 8.2 One automatic Video worker, two modes

- Triggered mode consumes admitted owner-bearing Video tasks and obeys its own interval.
- Continuous mode is independently enabled, schedule-gated, and has no owner or extra interval. `bRuleEnabled` is not its continuation condition.
- Enabling continuous immediately stops triggered admission, cancels pending triggered Video, and asks an active triggered clip to finalize with its original UID. Release that sink before creating a continuous one.
- Disabling continuous finalizes its UID-zero clip before permitting the next triggered task to start. Do not resurrect a pending task canceled during a previous mode.
- At schedule closure, request continuous finalization at the next bounded check; do not wait out 60 seconds or wait indefinitely for a keyframe. Reopening starts from a post-admission keyframe without recording the closed interval's backlog.
- Continuous entry/resume drops historical pre-roll. Normal adjacent continuous clips retain the boundary keyframe for the next clip rather than clearing the queue every time. Triggered clips retain their established pre-roll behavior.
- Bound waits for the first keyframe, post-start input loss, and the ending keyframe. Define interrupted/partial-file and failure outcomes with tests. Mode transitions, pause, and shutdown must break these waits.
- Preserve the 60-second target and keyframe-aware slicing; exact media duration and gap-free recording are not guaranteed. Synchronous finish/thumbnail work can exceed the approximately 10-second media buffer; measure gaps and memory before considering a separate post-processing mechanism.

### 8.3 Media-operation barrier and shutdown

Fix the barrier before introducing parallel execution:

1. Pause check and active-operation admission occur in the same critical section. Acquire an exception-safe operation guard **before** a fallible create call, so failed construction cannot leak the active count.
2. Cover automatic/manual video create/write/finish, image capture, device-dependent thumbnails, and Sound. Pure filesystem publication may finish after media is paused, but before storage/Context teardown.
3. Keep the existing balanced scoped suspend/resume counting; a waiting suspension closes admission before draining already-admitted operations.
4. Do not hold the global instance, configuration, executor, or callback-dependent resource locks while waiting for workers. Obtain a safe runtime reference and release the C API entrance lock before a potentially reentrant join or drain.
5. Stop in explicit dependency order: close service admission and stop the rule producer; cancel pending; stop/drain/join Sound, manual capture, and both automatic file workers; finish publication; then stop storage and release Context/runtime state. Do not rely on member-destruction order.
6. Keep callback pointers and `user_data` valid until all workers have joined. Notification callbacks must return promptly and must not synchronously call deinit, eject their own Slot, or perform another self-join operation.
7. Partial initialization must unwind every started thread and acquired resource.

Status 2026-10-19 (Phase 2B, first commit): items 1 to 3 are implemented by `Context::MediaBarrier` (`vigil/context.hpp`). Every writer, automatic or manual, image or video, holds one admitted operation from construction to destruction, which covers its media callbacks and the thumbnail generated when the Slot publishes its file; admission and pause share one mutex, a waiting pause drains through a condition variable, and the video sink is created only after the writer's pre-check passed, so a closed gate leaves no empty file. Pinned by `media_barrier_refuses_admission_while_paused_and_a_waiting_pause_drains` (`tests/test_context.cpp`) and `writers_hold_one_media_operation_from_construction_to_destruction` (`tests/test_writers.cpp`). Second commit: items 4 to 7. `vg_service_init` builds and `vg_service_deinit` destroys the runtime with no instance lock held (a lifecycle mutex serializes them, and the instance pointer is published or detached under the exclusive lock only), `vg_media_scoped_suspend_write` drains through a context reference without the entrance lock, `Vigil::~Vigil` stops the rule producer, cancels the pending event, then joins manual capture and storage before releasing the context, and a throwing constructor unwinds the members already built. Pinned by `tests/test_capi.cpp` (instance guard, nested suspension, reentry from the log callback during init and deinit). Phase 4B (2026-10-19): the Sound executor holds an operation while the callback runs and `Vigil::~Vigil` stops it right after the rule producer (`tests/test_sound_executor.cpp`). Review 2026-10-19: ALWAYS_ON now waits for every media operation (manual image capture, a publication's thumbnail, Sound), not only video writers; the operation spans the publication and the FILE callback, so a waiting suspension called from `on_file_event_cb` waits for itself (documented on the field; the operation is released before publication once the worker loop is extracted in Phase 4C); and `vg_media_scoped_suspend_write` counts only for the instance it captured (an instance generation), so a suspension racing a re-init cannot leave the new instance's count stale. Phase 4C-2 (2026-10-19): the thumbnail is generated under the writer's operation in `SlotAdaptedWriter::takePublication`, the worker releases the writer, and the Slot publishes with no operation held, so `on_file_event_cb` runs outside every media operation and the publication lock (the header note was updated; the self-wait no longer exists). Phase 4C-4 (2026-10-19): `Slot::enable` starts the manager and both channel workers and unwinds on a start failure (item 7), `disable` joins both workers before the manager, and `tests/test_capi.cpp` runs the lifecycle with the selected Slot's three threads (`the_selected_slot_starts_its_manager_and_both_channel_workers_and_deinit_joins_them`).

These guarantees are conditional on bounded callbacks and filesystem/device operations. Tests with fakes establish Vigil's synchronization, not the interruptibility of arbitrary drivers.

## 9. Storage and resource coordination

- Estimate simultaneous demand as at most one automatic video plus a photo, including pre-roll, GOP tail, thumbnails, and safety margin. Triggered and continuous Video are mutually exclusive, so do not budget them as two videos.
- Watermarks are estimates, not reservations. `.cache` in-flight files are not fully represented in the index. Register lightweight in-flight budgets under a short shared admission critical section, including manual capture on the same filesystem, and release them on every terminal path.
- Keep checking actual write errors and ENOSPC. Distinguish ordinary new-task headroom rejection from critical/device failure that requires stopping an active writer. Raising Photo's admission watermark must not terminate a healthy video. Decided 2026-10-19: admission uses the aggregate watermarks and the Slot state; a running task continues against the watermark bound into it at acceptance and stops for device or ENOSPC failure only (today `isReadyToWrite` is both the admission and the continuation condition, so saturation interrupts a running clip); the interruption revision compares a channel's settings without its watermarks (Phase 4C-3).

Status 2026-10-19 (Phase 4C-3): `WaterMarks` carries the bytes one task may write (`task_bytes`) and `WaterMarks::aggregate` sums two tasks, which `ActionSettings::create` uses as the effective watermarks when both channels are enabled. `Context::InFlightBudgets` keys reservations by filesystem (`utils::FilesystemIdOf`, the device number): `tryReserve` compares the free space minus the bytes in flight with the critical mark and reserves the task's own bytes in the same critical section; the Slot's `tryAdmit` reserves for the worker, which releases once the file is published, and `CaptureAdaptedWriter` reserves in its constructor and releases after its rename, so manual capture on the same filesystem counts. The channel revision moves on `FileWriterSettings::SameRecording` only (type, format, interval, prefix, clip length), so a refresh no longer interrupts a running task, and `SlotAdaptedWriter` continues on `FileChannel::isOpenFor` (rules, revision) and `Slot::canContinue` (not stopping, mounted and ready or saturated, free space above the task's own critical mark) instead of the Slot's readiness; the quota and the aggregate mark gate admission only. The manager's Ready/Saturated state and the HTTP precheck are unchanged. Pinned by `tests/test_record_config.cpp` (the aggregate, a watermark-only publish and a refresh moving the snapshot but no channel revision), `tests/test_context.cpp` (the budget arithmetic), and `tests/test_channel_worker.cpp` (a balanced budget after success, failure, cancellation, and interruption, a refused admission reserving nothing, and a raised admission mark that does not stop a running fake clip while a recording change does). Manual capture's reservation is covered by reading: it needs a real output directory.

Status 2026-10-19 (Phase 4C-4): with a worker per channel, admission is per task: `Slot::tryAdmit(admission, task_bytes)` checks the quota and the free space minus the bytes in flight against the aggregate mark, as the manager's Ready state does, and reserves the task's own bytes, so with both channels enabled the second task is admitted only when the free space covers the aggregate mark plus the first task's bytes (conservative by one task, as decided above). The state the two workers share was already atomic or mutex-protected (`_stats`, the budgets, `WriteStats`, the index, the publication lock); the cache of the internal preserved-space probe was not and is now under a mutex. On disable, switch, or removal, `Slot::disable` (or the destructor) joins both workers, then the manager, and the storage manager disables every other enabled Slot before enabling the selected one, so the replacement opens once the old consumers are drained; a task pending across the switch is canceled by the new workers when they start rather than moved, and the Slot identity on the task comes with Phase 4C-5. A thread that cannot be started unwinds the ones already running (`Slot::enable`), covered by reading: there is no injection seam.
- Replace destructive directory probing with idempotent creation: do not use `is_directory=false -> remove_all` against a path another publisher may be creating.
- Coordinate publication/rename with rotation, free-up, and explicit removals so a destination directory cannot disappear during publication. Do not run thumbnails or external callbacks under the coordination lock.
- Synchronize space-probe caches, counters, index updates, and shared state accessed by both workers. Keep tests around midnight directory creation and date-directory removal.
- Use collision-resistant automatic names with task/sequence identity and a no-overwrite publication policy. Timestamp plus extension is insufficient; UID-zero continuous clips need unique identities, including collision handling after restart.
- On Slot disable, switch, or removal, close admission and drain/join both consumers before opening the replacement. Tasks retain their original Slot and never move to another device. If the second worker fails to start, stop/join the first and unwind the partial enable.

Status 2026-10-19 (Phase 2C): `EnsureDirectoryExists` is idempotent and non-destructive, `Slot::moveAndAddCacheFile` generates the thumbnail before taking the Slot's publication lock and holds that lock through directory creation and the renames, the same lock covers rotation (`IndexManager::tryReleaseSpace`), free-up, and explicit removal, and `UniquePublicationPath` gives a taken name a numbered suffix instead of overwriting. Automatic names keep today's timestamp form; UID-zero continuous clips will publish through the same helper in Phase 4. The shared probe state was already atomic or mutex-protected (`_stats`, `WriteStats`, the index). Still open: in-flight budgets, the ENOSPC distinction, and the Slot-switch drain (Phase 4). Pinned by `tests/test_index_manager.cpp` (containment, non-destructive creation, the no-overwrite name); the lock itself needs a mounted Slot and is covered by reading only.

## 10. C API changes and compatibility boundary

In `include/vigil_ctypes.h`:

- Append `vg_sta_e (*sound_play_cb)(void *user_data)` at the end of `vg_config_media_t` with the bounded contract in section 7.3. Null means unavailable.
- Update `VG_CTYPES_VERSION` (currently `2026.09.15`) and `VG_CAPI_VERSION` (currently `2025.11.25`) and the header comments, including the corrected `source_delivery` wording.
- Append `int64_t dispatch_timestamp` at the end of `vg_rule_evt_t` (the owner's dispatch time; `timestamp` stays the event time) in the same phase, and document both fields.
- No new enum values; `vg_rule_type_e` and `vg_file_evt_t` are unchanged.

Status 2026-10-19 (Phase 4B): done. `sound_play_cb` is appended to `vg_config_media_t` with its bounded-completion contract, `dispatch_timestamp` to `vg_rule_evt_t`, both version strings read `2026.10.19`, the layout note is on `vg_config_media_t`, the module example sets the new callback and prints both times, and the test fakes record the new field.

This is still a **C ABI change**: tail-appending to `vg_config_media_t` shifts the following `storage` field in `vg_config_t`, the appended `vg_rule_evt_t` field is only readable by callers built against the new header, and the initialization API has no size/version negotiation. The library and callers built with the new layout must be rebuilt together. Do not claim that header version strings or tail-appending make old binaries compatible. Audit zero initialization, aggregate initialization, and field-by-field initialization in the module examples and tests.

Strings and event objects are borrowed for the duration of the callback; consumers that retain them must copy. Do not add an assumption that all callbacks run on one thread.

## 11. Five auditable implementation phases

Each row is a review unit, not a mandate to combine unrelated edits into one commit. Split further when needed. Every unit must build and have relevant tests; keep unsupported capabilities explicitly unavailable until their end-to-end path works. Reject a not-yet-supported capability in intermediate phases rather than accepting a no-op configuration write.

### Phase 1: Reproducible tests and configuration/ABI protection

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 1A | `CMakeLists.txt`, new module-owned `tests/` | Add optional, reproducible CTest coverage with fake-clock/callback support. Characterize current single-rule, input-reset, EVENT, and single-file-action behavior, including the pinned discrepancies in section 2. Status 2026-10-19: done as a baseline. Harness (`tests/support/`) and five executables (inference rules, schedule rule, context/events, record configuration, rule endpoint) pass in Debug, ASan/UBSan, TSan, and allocator-on builds. The rule-endpoint tests use a small seam: `RuleEndpoint(ctx, run_rule_thread=false)` starts no thread and `RuleEndpointTestAccess` (declared as a friend, defined only in the tests) runs one loop iteration. Not covered: a fake steady clock (Timer/ALWAYS_ON pacing beyond the same tick), SED, GPIO, TTY, the writers, and the C API entry points. |
| 1B | `vigil/types.hpp`, event projection, `example/` | Pin the C event projection with tests: type/id/source/delivery per section 6.1, exact UIDs above `2^53`, null/empty source. No public header changes in this phase. Status 2026-10-19: covered by `tests/test_context.cpp` (callback projection) and `tests/test_rule_endpoint.cpp` (rule-info JSON); no source change was needed, and `example/` is unchanged. |
| 1C | `vigil/configs.hpp`, `vigil/endpoints.hpp` | Add the version-2 parser and serializer with the strict `iSchemaVersion` check, store-then-publish commits, and startup fail-closed handling. Accept only single-file-action payloads until Phase 4D; reject the rest. Status 2026-10-19: done in one commit, kept small by assuming the premises above (latest schema only, complete documents, a trusted manager). `ConfigStore` fsyncs the temporary file before the rename and reports `ENOENT` for a missing file; `RecordConfig` reads complete documents and nothing else; `RuleEndpoint::updateRecordConfig` stores the canonical document and then publishes through `RecordConfig::sync` under the configuration mutex; startup keeps rules disabled when any file exists but cannot be applied. `docs/minimal-http-api.md` is at version 2.0.0 for the new payload. |

### Phase 2: Repair the single-writer foundation before splitting workers

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 2A | `vigil/context.hpp`, `vigil/writers.hpp`, `vigil/slot.hpp` | Identity-aware tasks, exception-safe accounting, explicit finish/publish before cooldown, uniform failure cleanup/backoff. Test old-A/new-B completion and every callback/publication failure with one writer. Status 2026-10-19, first commit: `SharedRuleEvent::claim`/`cancelPending`/`complete(expected)`; the writer thread claims the event, releases the writer (sink finished, file published) before the cooldown, completes only the event it ran, and treats a constructor failure as a failed task with the same cooldown and failure count. The channel still stays busy through the cooldown, as today. Second commit: an activity guard (replaced by `Context::MediaBarrier` in Phase 2B) balanced the writer count when the `VideoWriter` constructor threw; every writer exit path finishes the sink explicitly and at most once, a finish failure fails the task, and a writer that finds the gate closed before recording anything still reports Canceled, so nothing is published; and `tests/test_writers.cpp` runs both writers over a fake adapter with fake media callbacks (sink call sequence, every callback failure, interruption, activity count). Not covered: the Slot loop and cache-file publication, which need a mounted Slot. |
| 2B | `vigil/capi.cpp`, `vigil/vigil.hpp`, media-operation guards | Linearize pause/admission, cover images/thumbnails/manual capture, and fix runtime/worker stop order. Test reentry, overlapping suspension, drain, and partial initialization. Status 2026-10-19, first commit: the media barrier (section 8.3, items 1 to 3) with its tests. Second commit: the lifecycle (section 8.3, items 4 to 7) with `tests/test_capi.cpp`. Partial initialization has no injection seam; it rests on member construction order. |
| 2C | `vigil/slot.hpp`, `vigil/index_manager.hpp` | Non-destructive directory creation, unique publication names, synchronized probes, and publication/removal coordination. Test collisions, concurrent creation, and Slot removal. Status 2026-10-19: done (section 9 status); collisions and non-destructive creation are tested, concurrent creation rests on `create_directories` being idempotent, and Slot removal already joined the writer thread before unmounting. |

### Phase 3: Rule-side preservation and small fixes

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 3A | `vigil/context.hpp`, `vigil/rules.hpp`, `vigil/endpoints.hpp` | Update clocks before the enable check; stop inference timestamps overwriting the wall clock (the owner's two timestamps landed early on 2026-10-19, so the writer event can take its time from the owner); fill GPIO/TTY attribution; fix the TTY leak and overlapping-prefix matching; validate the GPIO mode callback; route HTTP activation through `dispatchRuleEvent` with body bounds and file-action-only storage prechecks. Everything else is pinned unchanged. Status 2026-10-19: done (section 5.2 status). |

### Phase 4: Action settings, Sound, two file channels, multi-action API

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 4A | `vigil/configs.hpp`, `vigil/context.hpp`, writer settings adapters | `ActionSettings` with per-channel revisions on the Phase 1 commit foundation. Preserve the single-writer behavior; test config/refresh conflicts. Status 2026-10-19: done in one commit (section 7.1 status). The snapshot is built before the commit lock, the Slot and ALWAYS_ON read the enabled file channel from it, and a configuration without a file action publishes a snapshot without one. |
| 4B | `include/`, runtime-owned Sound executor, C callback fakes, `example/` | Append the null-default `sound_play_cb` and the `dispatch_timestamp` field of `vg_rule_evt_t`, bump the version declarations, update initializers, and implement `Idle/Pending/Playing/Stopping` with pause/stop integration. Test null/error callbacks, Pending/Playing rejection, cancel-before-start, bounded in-flight completion, and no retry/backlog. Status 2026-10-19: done in one commit (section 7.3 and section 10 status). |
| 4C | split 2026-10-19 into 4C-1 to 4C-5 | Independent Photo/Video tasks, workers, budgets, and per-channel state. Preserve manual capture and `ALWAYS_ON` pacing. Test identity/settings isolation, simultaneous work, and partial worker startup. Each step below preserves today's single-action behavior and is testable without a device; section 13 orders them: per-channel state and budgets before two workers, `ALWAYS_ON` on the channel machinery before 4D. |
| 4C-1 | `vigil/context.hpp`, `vigil/rules.hpp`, `vigil/endpoints.hpp`, `vigil/slot.hpp`, `vigil/configs.hpp`, `vigil/vigil.hpp` | Per-channel task state with one worker and identical behavior: replace `SharedRuleEvent` with one `FileChannel` per `CaptureType` (`tryAccept(owner, settings, slot)`, `claim`, `cancelPending`, `requestStop`, `complete(expected)`, hold-until, acceptance sequence, last/current); rename the dispatch to `dispatchRuleEvent`, which accepts into every channel the snapshot enables (none can enable two before 4D) and records the last dispatched owner; the Slot thread claims from the enabled channel; `AlwaysOnRule`, rule info (by acceptance sequence), the HTTP result, `tick`, `~Vigil`, and `sync` (a channel's settings change cancels that channel's pending task) move to the channel API. Acceptance: every existing case passes with helper renames only; new cases pin identity per channel (old A never resets new B), dispatch over a two-channel snapshot built directly (`writer_created` true if either accepted), `ALWAYS_ON` waiting through the hold, and unchanged rule-info fields and HTTP codes for one channel. Status 2026-10-19: done in one commit (section 7.2 status). Deviations: the Slot identity and `requestStop` wait for their first users (4C-5 and 5A); the Context-level tests publish one Video channel before dispatching, since acceptance is per enabled channel (a bare context accepted every dispatch before); the existing cases otherwise changed by renames and by reading the event through the task. |
| 4C-2 | `vigil/slot.hpp`, a worker header, `tests/` | Extract the per-task loop of the Slot writer thread into a `FileChannelWorker` over an adapter (readiness, quota and space probes, failure mark, writer factory), still one worker per Slot, with the cooldown and failure limit as constructor parameters defaulting to today's constants; the publication is released from the media operation before the rename and the FILE callback. Acceptance: a new `tests/test_channel_worker.cpp` over a fake adapter and `FakeMedia`: claim, write, publish before the hold, complete; the cooldown and the stop after N failures with injected constants; Canceled and Interrupted do not hold; old A/new B through the loop. Status 2026-10-19: done in one commit (section 7.2 status); `FakeMedia` and the fake writer adapter moved to `tests/support/fake_media.hpp`, shared with the writer tests. |
| 4C-3 | `vigil/types.hpp`, `vigil/context.hpp`, `vigil/writers.hpp`, `vigil/endpoints.hpp` | Budgets and the healthy-work rule: aggregate watermarks (one Video plus one Photo), an in-flight registry keyed by filesystem covering automatic Photo and Video and manual capture, released on every terminal path; the interruption revision without watermarks; continuation bound to the task's own watermark. Acceptance: `tests/test_record_config.cpp` pins the aggregate and that a watermark-only change moves admission but not the interruption revision; the worker tests pin a balanced budget after success, failure, cancel, and interruption, and a raised admission mark that does not stop a running fake clip. Status 2026-10-19: done in one commit (section 9 status); the fake writer adapter now watches its channel's gate when built from a task, as the Slot's does. |
| 4C-4 | `vigil/slot.hpp`, `vigil/storage.hpp` | Two workers per Slot, one per channel with its own failure count; `enable()` unwinds the first worker when the second fails to start, `disable` joins both, either limit marks the Slot failed (retained limitation), and the Slot switch path still disables before enabling. Acceptance: the C API runtime starts and stops under TSan with both threads; partial worker startup has no injection seam and is covered by reading. Status 2026-10-19: done in one commit (section 7.2 status); the `vigil/storage.hpp` change documents the switch order, which already disabled before enabling. |
| 4C-5 | `vigil/context.hpp`, `vigil/endpoints.hpp`, `vigil/slot.hpp` | Admission at dispatch, the one deliberate change of section 8.1: the gates (`bRuleEnabled`, the schedule flag published before the enable check, media pause), an idle channel with its hold elapsed, a usable Slot through a per-channel admission interface registered by the Slot (replacing `slot_check`), and budget headroom, all in one critical section; rejected file actions leave nothing pending. Acceptance: without a usable Slot the owner is reported with `writer_created=false` and the next owner proceeds; a gate closed between evaluation and dispatch is rejected; the schedule flag is published while rules are disabled; single-action HTTP codes unchanged. |
| 4D | `vigil/configs.hpp`, `vigil/endpoints.hpp`, dispatch/info | Enable version-2 multi-action configuration and canonical round trips. Validate both POSTs and startup against the ALWAYS_ON invariant under one commit mutex taken by both POSTs after parsing and before any store (the two configuration mutexes are otherwise an ABBA pair, and the rule configuration has no revision, so the check is against the committed state); at startup validate the pair after both files load and fail closed on a violation, leaving both files in place and reporting both stored documents. The invariant: with `ALWAYS_ON` selected, at most one file channel, Sound disabled, continuous disabled, and `bRuleEnabled=true` additionally requires exactly one channel; the first-start state (`ALWAYS_ON`, zero channels, rules disabled) satisfies it. Exercise Photo+Video+Sound and same-UID multiple FILE callbacks with fakes; Sound-only and rule-only work without storage. Still reject `bContinuousRecordEnabled=true`. |

### Phase 5: Schedule-aware continuous video and final review

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 5A | Video worker, clocks/gates, action configuration | Enable continuous recording. Test `bRuleEnabled=false` with the schedule open, schedule-closure finalization, bounded keyframe/input-loss waits, UID-safe mode handover, and no stale pre-roll or overlapping automatic sinks. Needs from 4C (decided 2026-10-19): a task kind (triggered or continuous) with a kind-specific continuation predicate (`bRuleEnabled` is not continuous mode's), `requestStop` on a running task, owner-less acceptance for UID-zero clips that counts as no rule event, one Video budget slot for either mode, a buffer-preparation hook before writer construction (triggered clips keep today's pre-roll of up to the buffer period from the oldest keyframe; continuous entry drops it), and the bounded-wait constants of section 13. |
| 5B | Module docs, public comments/examples, complete tests | Regress single-action MP4/JPG/RAW and intervals, finish API/docs updates, run standalone and sanitizer checks, and report limitations and unverified callback capabilities. Update the TODO checklist with evidence. Documentation rows from the 2026-10-19 review: the `bReadyForNewEvent` prose, the triggered pre-roll, and the empty-clip outcome. |

The public struct layout still has a compatibility dependency (section 10): do not suggest swapping an old library against new structs. Keep version-2 fixtures for every phase.

## 12. Deterministic validation matrix

Tests must execute production rule, configuration, executor, and writer code. Use a controllable clock, scripted inputs, fake media callbacks, temporary filesystems, and explicit thread barriers. Avoid sleeps as correctness evidence.

| Group | Required scenarios | Key assertions |
| :--- | :--- | :--- |
| R1 Rule preservation | First-match `InferenceRuleSet`; debounce counter including zero; missing/unrelated/negative input; queue reset; EVENT bypass; gate close/reopen. | Behavior identical to today; only matching FRAME input changes the counter; EVENT is not a negative FRAME. |
| R2 Peripherals | GPIO level/edge and mode-callback validation; TTY leak, overlapping prefixes, fragmented commands, continuous bytes; SED silence/disconnect/ignored packets/empty class filter. | Fixes limited to the named defects; everything else pinned. |
| R3 HTTP/EVENT | Activation with all actions off, Sound-only, missing storage, busy video, oversized body, concurrent configuration commit. | `code=0`/`EBUSY` per section 5.2 with the single-action case unchanged; storage gates only file actions; prompt errors. |
| A1 Independent actions | Video busy while Photo/Sound trigger; all actions off; Sound-only; continuous configured. | Independent admission; permitted RULE still reports; `writer_created` is file acceptance only. |
| A2 Task identity | A running, gate closes/reopens, B admitted, A completes; Slot replacement. | A cannot clear/restart B or migrate owner/settings/device identity; canceled pending work never starts. |
| A3 Completion/errors | Short/long interval; create/write/finish/rename failures; nonzero positive callback statuses. | Publish before cooldown; correct terminal result; no false FILE success; balanced guards/budgets and bounded backoff. |
| A4 Settings | Photo/Sound-only changes, Video refresh, admission watermark increase. | Only related channels invalidated; both video-mode settings refreshed; healthy work not stopped by new headroom alone. |
| A5 Pause/stop | Admission racing pause; image/thumbnail/manual video/Sound in flight; nested pause; callback reentry; partial worker startup. | No old-resource operation after drain; no lock cycle, self-join, leaked activity, orphan worker, or dangling callback data. |
| A6 Continuous | Rule disabled, schedule boundaries, both mode switches, missing first/last keyframe, input loss, manual capture overlap. | One automatic sink, bounded stop, correct UID, no stale pre-roll, correct boundary-frame/PTS handling. |
| A7 Storage | Midnight directory race, rotation/removal during publish, naming collision, ENOSPC, Slot removal, same-filesystem manual capture. | No destructive create/overwrite, released reservations, no false publication success, safe device lifetime. |
| S1 Sound states | Null callback, callback error, Pending/Playing retrigger, disable/stop before and during execution. | No backlog or cooldown; no retry of a failed trigger; acceptance/stop linearized; join waits only for bounded admitted work. |
| C1 Schema | Missing or other `iSchemaVersion`, the old `dWriterConfig` shape, missing fields, wrong types, zero/fraction/overflow intervals, not-yet-supported capabilities in intermediate phases. | Canonical GET/save; precise rejection; a rejected file is never rewritten. |
| C2 Commit and startup | Revision conflict, pre-rename failure, missing files, corrupt rule/action/schedule files, unimplemented capability in an intermediate phase. | No partial flags; old state preserved on failure; retained invalid files; no default ungated/ALWAYS_ON execution; no success-without-behavior. |
| C3 ALWAYS_ON | MP4/JPG/RAW, enable/schedule combinations, intervals, both POST orders and concurrent POSTs. | No automatic continuous conversion or format/pacing change; single-file-action invariant cannot be bypassed. |
| W1 Event boundary | All enum types, direct EVENT, null/empty source, UID above `2^53`, event and dispatch times. | Attribution per section 6.1, delivery `EVENT` for source-less owners, exact integers, input time as the event time for FRAME/EVENT owners, no trigger/action arrays. |
| W2 File association | Photo+Video from one owner, failures with zero successful files, FILE before RULE, several continuous UID-zero clips. | Task-bound UID stable; no promised count/order; file identity distinct from event UID. |

### 12.1 Reproducible module-only build and test setup

Vigil builds and runs on Linux only: it uses fanotify, netlink uevents, libblkid, `statvfs`, and Unix domain sockets. The macOS host that holds this checkout cannot compile it. Build and run in the `dev` container instead.

**Dev container.** Reach it with `ssh dev` (Debian 13 "trixie", aarch64, 7 cores, 11 GiB RAM, root). The checkout is mirrored at `~/dev/Vigil` (`/root/dev/Vigil`) over virtiofs: host edits are visible immediately and the `.git` directory is shared, so do not run git operations on both sides at the same time, and keep build output out of the mirror. Toolchain verified on 2026-10-19: GCC 14.2.0 (no clang), CMake 3.31.6, Ninja, pkg-config 1.8.1, `protobuf-compiler`/`libprotobuf-dev` 3.21.12, `libblkid-dev` 2.41.5, and `autoconf` for the bundled jemalloc; the last two were installed for this review. GCC's ASan, UBSan, and TSan runtimes are available.

- The source build requires CMake 3.21+, a C++20 compiler, Protobuf/protoc, PkgConfig, libblkid, libatomic, nlohmann/json, and git for the bundled submodules. Protobuf is required even when `VIGIL_USE_SYSTEM_PROTOBUF` is off; outside Buildroot mode that option changes nothing, and there is no bundled Protobuf build.
- `VIGIL_USE_SYSTEM_NLOHMANN_JSON` defaults off; a system package must be at least 3.12.0 (a `find_package` minimum, not an exact pin). The bundled `3rdparty/nlohmann_json` submodule is 3.12.0 (commit `55f9368`). When the bundled copy is missing, CMake runs `git submodule update --init` inside the source tree, which needs network access and modifies the mirror; initialize submodules deliberately before offline validation.
- Jemalloc defaults on and replaces global C++ allocation. Bundled mode needs the `3rdparty/jemalloc` submodule (commit `81034ce`), `make`, and `autoconf`, and adds roughly 50 s to a clean build. Keep dependency and allocator changes out of this refactor unless necessary and separately reviewed.
- Use a **fresh** build directory for every configuration, out of tree such as `/root/build/vigil-host`, or git-ignored `build/<name>` inside the mirror. `.gitignore` ignores only `build/`, so an in-tree `build-refactor-host/` would appear as untracked noise. Never edit a reused cache; record the compiler, dependency sources, and allocator options with every result. Standalone inference generation uses `protocol/inference.proto`.
- Source-defined targets are `vigil`, plus `vigil_example`, `vigil_index_manager_test`, and `vigil_mask_test` when `VIGIL_BUILD_EXAMPLES=ON`. Building them checks compilation; do not automatically run interactive/privileged examples as tests.
- Module tests live in `tests/` behind `VIGIL_BUILD_TESTS` (default OFF, added in Phase 1A on 2026-10-19). `tests/CMakeLists.txt` registers one CTest entry per executable; each is a self-contained program built on `tests/support/check.hpp` (no external framework) and `tests/support/fake_config.hpp` (a `vg_config_t` whose only live callbacks are logging and rule-event recording). Executables are written to `<build>/tests/`; tests that need configuration files use a fresh `mkdtemp` directory under the system temporary directory and remove it afterwards. A fresh CTest listing must contain the intended cases; zero discovered tests is not success.
- Verified baseline, 2026-10-19, in the container (Debug, allocator off):

  ```sh
  cmake -S ~/dev/Vigil -B /root/build/vigil-host -G Ninja \
      -DVIGIL_USE_BUILDROOT_SYSROOT=OFF -DVIGIL_BUILD_EXAMPLES=ON \
      -DVIGIL_ENABLE_JEMALLOC=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build /root/build/vigil-host -j7          # 16 steps, about 7 s, exit 0
  ctest --test-dir /root/build/vigil-host -N        # Total Tests: 0
  mkdir -p /root/build/run-example && cd /root/build/run-example
  /root/build/vigil-host/vigil_example              # 5 s demo with dummy callbacks, exit 0
  ```

  The same configure with `-DVIGIL_ENABLE_JEMALLOC=ON` (`/root/build/vigil-host-jemalloc`) also builds and runs the demo. The allocator-off configuration is the logic-test baseline; validate the allocator-on configuration separately (on 2026-10-19 the Phase 1A tests also passed 5 of 5 in a fresh allocator-on build, `/root/build/vigil-host-tests-jemalloc`).
- Verified test run, 2026-10-19, in the container (Debug, allocator off):

  ```sh
  cmake -S ~/dev/Vigil -B /root/build/vigil-host-tests -G Ninja \
      -DVIGIL_USE_BUILDROOT_SYSROOT=OFF -DVIGIL_BUILD_EXAMPLES=ON -DVIGIL_BUILD_TESTS=ON \
      -DVIGIL_ENABLE_JEMALLOC=OFF -DCMAKE_BUILD_TYPE=Debug
  cmake --build /root/build/vigil-host-tests -j7                     # about 10 s, exit 0, no warnings
  ctest --test-dir /root/build/vigil-host-tests -N                   # Total Tests: 5
  ctest --test-dir /root/build/vigil-host-tests --output-on-failure  # 100% tests passed, 0 tests failed out of 5
  ```

  After Phase 4C-4 (2026-10-19) the eleven executables hold 100 cases (inference rules 12, schedule rule 7, context 17, record configuration 13, rule endpoint 16, writers 5, C API 5, index manager helpers 3, peripheral rules 4, sound executor 8, channel worker 10). `vigil_test_capi` runs the real runtime (rule, storage, and capture threads, plus the selected Slot's manager and both channel workers) with a nonexistent internal device and every scanned block device hidden, so it still touches no device.
- Sanitizers: `-DCMAKE_C_FLAGS`/`-DCMAKE_CXX_FLAGS` of `-fsanitize=address,undefined -fno-omit-frame-pointer` (`/root/build/vigil-host-asan`) and, separately, `-fsanitize=thread` (`/root/build/vigil-host-tsan`) both build with the allocator off. On 2026-10-19 `vigil_mask_test` and the demo ran under both with no reports, which only proves the toolchain works: the demo exercises no rules, inputs, or writers. The Phase 1A test executables also pass under both (`/root/build/vigil-host-tests-asan` and `/root/build/vigil-host-tests-tsan`, examples off, 5 of 5 each). Keep ASan/UBSan and TSan in separate configurations, run them with the allocator off, and document allocator/sanitizer limitations rather than hiding failures. Ordinary unit tests should need neither real peripherals nor elevated filesystem-monitor privileges.
- Distinguish host logic correctness from target timing/media capability. Report actual evaluation delay, callback bounds, memory/CPU use, and clip gaps only when measured. Do not claim hardware success, hard real-time bounds, or gap-free recording from fake callbacks.

## 13. Review gates and return package

Before implementation, fix the test constants for finite media waits and per-channel failure backoff, test their boundaries, and record them without expanding configuration. Owners (decided 2026-10-19): the failure cooldown and limit become constructor parameters of the worker extracted in Phase 4C-2, with the production defaults `kWriterFailureCooldown` and `kWriterConsecutiveFailureMax` (done 2026-10-19: `FileChannelWorker`, the defaults in `vigil/constants.hpp`, both pinned with injected values); the keyframe and input-loss waits land in Phase 5A, which also decides the outcome of a clip that saw no keyframe before its length elapsed (today an empty file is published as Completed; untested).

Before enabling a phase's capability, its prerequisite task ownership, startup/commit safeguards, and callback availability must be in place. Do not advertise Sound as safely stoppable without a bounded callback contract. Do not enable multiple file tasks before independent channel state, shared filesystem coordination, and identity-aware finalization exist.

Return the refactored module with:

1. A concise, ordered change/commit summary mapped to the five phases and TODO identifiers.
2. Updated local API/architecture documentation, version declarations, configuration fixtures, and module examples.
3. Reproducible build/test commands and results, including test discovery, failure-injection coverage, sanitizer limitations, and explicitly unrun validation.
4. A list of contract deviations, unresolved compatibility edge cases, and callback/performance limits needing review.
5. An updated `docs/TODO.md` with evidence for completed items and explicit blockers for unfinished work.

Keep the return package module-local. This document is a handoff specification, not an authorization to expand the implementation beyond Vigil.
