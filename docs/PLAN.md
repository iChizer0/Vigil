# DRAFT PLAN

## 1. Goals and fixed decisions

1. Separate rule evaluation from post-trigger action admission and execution.
2. Add `MIXED`, an OR of distinct supported rule types, at most one child per type. Evaluate every child on every enabled, schedule-open tick, including every member of an `InferenceRuleSet`.
3. Select one winning owner through explicitly **lossy** coalescing. Full evaluation advances state; it does not guarantee reporting every activation or fair attribution.
4. Use `Inactive / OnHold / Active` results and one bounded pending candidate. Equal priority retains the earliest owner; higher priority replaces it without extending the deadline.
5. Start with a **500 ms arbitration budget**. This is not an inference-input timeout, debounce reset policy, or hard real-time guarantee.
6. Admit Photo (JPG/RAW), triggered Video (MP4), and Sound independently. Photo and Video have independent intervals; Sound has an enable switch only, no business cooldown or backlog.
7. Add independent continuous MP4. It ignores `bRuleEnabled` but **obeys schedule**. While configured, it suppresses triggered Video without changing the saved triggered-video enable flag.
8. Keep direct discrete EVENT delivery. Evaluate the configured tree on the same tick and, if both activate, publish two RULE events, each with its own action-admission attempt.
9. Keep legacy `ALWAYS_ON` as a single-file-action compatibility mode. Never automatically convert it to continuous MP4 or change JPG/RAW into video.
10. Keep RULE events simple: one winner's `type`, `trace`, `id`, and source. Do not add `triggers[]` or action-result `actions[]`; `dActions` is a configuration dictionary only.

### Non-goals

- No nested MIXED, AND expressions, multi-candidate queues, attribution-fairness scheduler, or scene deduplication.
- No lossless GPIO/input/notification guarantee, durable notification bus, or generic action thread pool.
- No new source epochs, per-source debounce partitioning, source-trust taxonomy, or general input-staleness policy.
- No media-device implementation, playback resource parser, routing policy, or application event store inside Vigil. Their relevant boundary is the public callback contract, exercised locally using fakes.
- No unrelated storage/capture API redesign, dependency overhaul, or build-cache replacement.

## 2. Current module baseline and reading map

The current flow is a selected `BaseRule`, a single `SharedRuleEvent`, and one automatic Slot writer. `Context::tryCreateWriterEvent` couples file admission to rule reporting. `AlwaysOnRule` uses the same selected-rule and writer path. The refactor must not simply add two threads to this shared state.

| Area | Read first | Relevant current limitation |
| :--- | :--- | :--- |
| Rule implementations | `vigil/rules.hpp` | `InferenceRuleSet` returns at its first match; several rule types share the same precedence; HTTP dispatches directly. |
| Inputs, clocks, admission | `vigil/context.hpp` | One shared file task; identity-free reset; rule disable returns before clock update; input time can overwrite current wall time. |
| Configuration | `vigil/configs.hpp`, `vigil/endpoints.hpp` | Single `dWriterConfig`; enablement may change before revision validation; rule replacement destroys the old instance first. |
| Endpoint loop and startup | `vigil/endpoints.hpp` | EVENT bypasses configured-tree evaluation; startup load failures may leave defaults; temporary-file rename lacks complete durability handling. |
| Identity/settings | `vigil/types.hpp` | `EventOwner` has no trace; `WriteEvent` and `FileWriterSettings` are useful building blocks. |
| Writers and publication | `vigil/writers.hpp`, `vigil/slot.hpp` | Constructor failure can leak activity accounting; final publication depends on destruction and may happen after cooldown. |
| Filesystem coordination | `vigil/index_manager.hpp`, `vigil/slot.hpp` | Directory creation may remove a concurrent publisher's directory; publication and removal need shared coordination. |
| C API and lifetime | `include/vigil_ctypes.h`, `include/vigil_capi.h`, `vigil/capi.cpp`, `vigil/vigil.hpp` | No trace/playback callback or layout negotiation; pause/admission and shutdown joins need stronger lifetime rules. |
| Input schemas | `protocol/inference.proto`, `3rdparty/acousticslab_proto/` | Preserve FRAME/EVENT interpretation and SED input framing; do not invent another ingress protocol. |
| Build and examples | `CMakeLists.txt`, `README.txt`, `example/` | C++20 library target `vigil`; examples are not a deterministic regression suite. |
| Existing documentation | `docs/minimal-http-api.md`, `docs/architecture.dot` | Describe the current API and architecture, with the discrepancies listed below. |

Current defaults worth preserving unless explicitly changed here: 10 ms target rule-loop period, inference queue capacity 60 with oldest-entry dropping, 128 x 128 ROI masks, 60-second target video clips, approximately 10 seconds of keyframe-aware media buffering, and a 5000 ms writer interval.

### Known documentation/implementation discrepancies

These are separate compatibility decisions, not permission to make incidental behavior changes:

| Topic | Current discrepancy | Required treatment |
| :--- | :--- | :--- |
| Inference debounce | API prose says consecutive frames; implementation increments/decrements a saturating counter. | Preserve the counter algorithm and correct the explanation; absent/unrelated input is not negative input. |
| Debounce zero | Zero can make a negative input appear active. | Version-2 rules should reject zero and use one for no debounce; report legacy zero explicitly and review its handling before conversion. Validate the `int` upper bound. |
| Recording interval zero | Documentation permits it; the parser currently rejects `<= 0`. | Explicitly support nonnegative version-2 intervals with tests; zero removes extra cooldown, not serialization. |
| Empty SED class filter | Documentation says match any label; implementation matches none. | Pin current behavior with a test and document the compatibility choice before changing it. |
| Direct EVENT ID | Documentation describes source-as-condition-ID; implementation uses an empty ID and separate source ID. | Keep the empty condition ID and separate source in the new contract. |
| Rule info | Existing last-event prose and current-task selection differ. | Introduce independent last-dispatched owner and clearly distinguish it from channel task state. |
| TTY matching | Architecture text describes a different matcher. | Test actual streaming prefix behavior, then update the diagram. |
| Media suspension | Header wording is broader than current video-only activity accounting. | Implement the complete media-operation barrier described in section 8. |

Current source has no first-party `tests/` directory or CTest registration. `vigil_example` is a demonstration; `vigil_index_manager_test` is an interactive filesystem monitor; `vigil_mask_test` prints examples without assertions. Generated test binaries/registrations that may remain in `build/` are not reproducible evidence and must not be reused as the test source of truth.

## 3. Target architecture and responsibilities

| Layer | Responsibility | Must not own |
| :--- | :--- | :--- |
| Configuration/endpoints | Parse, validate, prepare, commit immutable settings; expose capabilities/status; publish gate transitions. | Rule arbitration state, blocking playback, worker joins under configuration locks. |
| Rule evaluation | Advance all rule state; consume inputs; produce one attributed result for the configured tree. | File availability, action cooldowns, storage I/O, playback, or direct HTTP-thread dispatch. |
| Action dispatch/execution | Independently admit immutable Photo/Video/Sound tasks; own worker state, settings, budgets, finalization, and cancellation. | Deciding which rule won or changing a captured owner's identity. |
| Public C API/lifetime | Validate inputs, maintain runtime lifetime, expose borrowed event data, and invoke bounded media callbacks. | Device-specific playback or a guarantee that arbitrary callbacks can be forcibly canceled. |

The normal path is full evaluation, winner attribution, release of rule/configuration locks, independent channel admission, and RULE callback. FILE callbacks follow successful publication by the file workers. Continuous video starts internally in the one Video worker and does not fabricate RULE events.

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

- `iSchemaVersion=2` identifies the new shape. An unversioned old shape is legacy; an unknown future version is rejected and never rewritten.
- `bRuleEnabled` is required and Boolean. `bContinuousRecordEnabled` defaults to false. A first start without configuration must not enable automatic behavior.
- `dActions` is required for the new shape. Missing child objects mean disabled actions. Photo defaults to `bEnabled=false`, `sFormat="JPG"`, `iIntervalMs=5000`; Video to disabled and 5000 ms; Sound to disabled.
- Existing fields of the wrong type are errors, not missing values. Photo is JPG or RAW; Video is always MP4. Sound has no interval, file path, playback queue, or per-trigger payload in this design.
- Photo/Video intervals are nonnegative integers with checked conversion and time arithmetic. Reject fractions, negatives, and overflow. Define/test resource limits for rule lists, strings, buffers, and request bodies as well.
- The earliest next start is `max(actual_start + interval, finalization_completion)`, subject to current gates. An interval shorter than the write duration introduces no additional delay; zero does not allow two simultaneous tasks in a channel.
- `bContinuousRecordEnabled=true` disables triggered Video admission even when continuous recording is outside schedule, paused, or unable to access storage. Preserve the saved `dVideo.bEnabled` value.

Legacy import maps `dWriterConfig` to exactly one file action: MP4 becomes Video; JPG/RAW becomes Photo with the same format. Preserve the old interval and `bRuleEnabled`; disable Sound and continuous recording. Do not special-case `ALWAYS_ON` into a new mode.

GET and persistence return canonical versioned JSON, not an ambiguous mirrored `dWriterConfig`. Reject payloads containing both old and new shapes, and never fall back to legacy parsing after a malformed new shape. Legacy writes are allowed only for an explicit import or a still-equivalent single-file-action configuration. Reject a legacy whole-object replacement once MIXED, multiple actions, Sound, or continuous recording would be lost.

### 4.2 Selected rule and MIXED configuration

`rule/record/config`, persisted as `record_rule_config.json`, gains version 2 and `lMixedRules`. Reuse the existing top-level child configuration blocks. This complete example uses three different child types:

```json
{
    "iSchemaVersion": 2,
    "sCurrentSelected": "MIXED",
    "lMixedRules": ["INFERENCE_SET", "GPIO", "HTTP"],
    "lInferenceSet": [
        {
            "sID": "person_detection",
            "lSourceFilter": ["camera"],
            "iDebounceTimes": 3,
            "lConfidenceFilter": [0.5, 1.0],
            "lClassFilter": ["person"],
            "lRegionFilter": []
        }
    ],
    "dGPIO": {
        "sName": "GPIO_01",
        "sState": "FLOATING",
        "sSignal": "RISING",
        "iDebounceDurationMs": 100
    }
}
```

Names in examples are illustrative and must resolve against the configured inputs/peripherals. The rules are:

- `lMixedRules` is a nonempty array of unique strings from `INFERENCE_SET`, `SED`, `GPIO`, `TTY`, `TIMER`, and `HTTP`. There is no direct `INFERENCE` child: use `INFERENCE_SET`.
- Reject duplicate types, nested `MIXED`, `SCHEDULE`, `ALWAYS_ON`, `UNKNOWN`, unsupported types, and invalid active child blocks with the existing invalid-argument error convention.
- Blocks are `lInferenceSet`, `dSED`, `dGPIO`, `dTTY`, and `dTimer`; HTTP has no extra configuration. Preserve inactive blocks when saving. Validate all activated blocks before changing the live tree.
- List order is for presentation, not priority. Use one explicit rank table: `INFERENCE_SET > SED > GPIO > TTY > TIMER > HTTP`, highest priority first. The existing `_precedence` values do not encode this total order.
- Single-rule selections remain available. `schedule_rule_config.json` remains a separate gate with its established local-time semantics; a disabled schedule gate is open.

### 4.3 Immutable commits, replacement, and startup

1. Parse and validate the requested configuration and its combination with the current related configuration. Prepare immutable action settings and fallible resources before publication where possible. Do not call media callbacks under the commit lock.
2. Serialize configuration writers and media-parameter refresh. Under the commit protocol, recheck revisions before replacing the file or changing any runtime flag. A conflict must leave both unchanged.
3. Write and fsync the temporary file, then rename it. Any failure **before successful rename** preserves the old file and old runtime snapshot.
4. Successful rename is the visibility boundary: publish its corresponding preprepared runtime snapshot without a fallible second application step. Fsync the parent directory as part of the durability protocol.
5. If directory fsync fails after rename, report **applied, durability unconfirmed**. Runtime and the visible file remain the new version; GET must expose enough information to verify this outcome. Do not claim no change, blindly retry, or silently roll back to another uncertain disk state. Block subsequent persistent commits until directory synchronization succeeds or startup revalidation establishes the state.
6. The two configuration POSTs are not a multi-file transaction. They share serialized combined-state validation, especially the `ALWAYS_ON` invariant below. Do not claim that two independent file renames are atomic as a group.
7. Rule resources need explicit handover: construct a replacement first when resources can coexist; release/reacquire exclusive GPIO resources in a defined order when they cannot. Clean partial construction with RAII. Failed restoration is observable and disables the affected trigger path; it must not activate default `ALWAYS_ON`.
8. Best-effort reconstruction of an old rule configuration does not restore its exact counter, timer, connection, subscription, or pending state. Test and document possible input gaps. Clear old arbitration and activation latches on replacement.
9. At startup validate action, selected-rule, and schedule configuration before opening automatic admission. Missing files use safe disabled defaults; corrupt or unsupported existing files are retained with an error and affected automation closed. A bad schedule must not become an always-open gate.

### 4.4 Legacy ALWAYS_ON invariant

Keep `ALWAYS_ON` as an adapter over **one enabled file channel**, Sound disabled, and continuous disabled. Its interval, MP4/JPG/RAW format, rule-enable gate, schedule admission, and RULE/FILE UID association remain meaningful. Do not turn it into a multi-action scheduler or remove gaps caused by its interval.

Both `rule/config` and `rule/record/config`, plus startup, validate this invariant against the other configuration's current revision in the shared commit protocol. Concurrent or differently ordered POSTs must not bypass it. To configure several actions, select a supporting rule such as TIMER or MIXED first; to switch back, simplify the actions first. Implement this adapter **before** exposing the multi-action API, not as a final cleanup.

## 5. Rule evaluation and bounded arbitration

### 5.1 Evaluation result and tick order

The conceptual result is `EvalResult { EvalStatus status; shared_ptr<EventOwner> owner; }`:

| Status | Meaning | Owner |
| :--- | :--- | :--- |
| `Inactive` | No activation and no unfinished confirmation eligible to delay another candidate this tick. It does not necessarily mean negative input. | Null |
| `OnHold` | An unfinished confirmation may delay a lower-priority candidate. | Null |
| `Active` | The rule has consumed/observed an activation. | Non-null |

Full evaluation does not require three-valued Boolean logic. The three-state result exists only to support the agreed bounded preference for a higher-priority confirmation window.

Each tick:

1. Update steady time, current wall time, and schedule state before considering `bRuleEnabled`. Continuous operation requires current time even when rule evaluation is disabled.
2. Obtain at most one queued inference input using the existing bounded-queue policy. No input must not prevent non-inference rules from being evaluated. Preserve FRAME subscription behavior and direct EVENT intake even without inference-rule subscribers.
3. When rule enablement and schedule allow, evaluate every configured child and every inference-set member once against the same tick input. Catch/report a recoverable child error as no result for that child, then continue.
4. A direct EVENT is not a negative FRAME. Capture its owner separately while still evaluating the configured tree.
5. Finish attribution, release rule/configuration locks, recheck admission gates, then dispatch the direct EVENT first and the configured winner second. Their owners/UIDs are separate. One may occupy a channel before the other, but both still attempt RULE reporting if their gates remain open.

Keep evaluation free of blocking connect calls, unbounded socket drains, storage operations, and playback. Notification callbacks must return promptly, but the 10 ms target remains a measured scheduling objective, not a guarantee.

### 5.2 Leaf state contracts

**InferenceRule:** maintain business debounce separately from arbitration hold eligibility.

| Input this tick | Counter update | Result |
| :--- | :--- | :--- |
| Positive FRAME from a matching source | Increment and saturate using the existing algorithm. | `Active` at threshold; otherwise start/retain `OnHold`. |
| Negative FRAME from a matching source | Decrement by one, not reset to zero. | `Inactive`; end hold eligibility even if the counter remains nonzero. |
| No input, unrelated source, or direct EVENT | No change. | `OnHold` only if an earlier unfinished positive confirmation is still eligible; otherwise `Inactive`. |
| Empty tick after confirmation | No change and no new owner. | `Inactive`; confirmed residual state is not a fresh hold. |

There is **no 500 ms no-input reset**. Test slow sources with intervals longer than the hold budget, for example 960 ms. `resetInference(source)` / `vg_inference_queue_reset` clear queued input only, not counters, already-consumed history, or pending ownership. Gate closure separately ends hold eligibility without clearing the counter. Source filtering remains a filter, not per-source counter partitioning.

`InferenceRuleSet` evaluates all members, then selects the first `Active` in `lInferenceSet` order. Any Active outranks all OnHold results within this set. With no Active it returns OnHold if any member is holding, otherwise Inactive. Only the selected owner receives one `INFERENCE_SET` trace prefix.

**SED:** keep the existing consecutive-time-window algorithm and bounded input framing. The input worker and evaluator share a short-lock, consistent snapshot of a latched activation and an unfinished confirmation. Consume Active before considering OnHold. A successful confirmation must not remain holding solely because `active_since` is nonempty. Negative input, disconnect, and gate closure end unfinished confirmation. Ignored packets do not advance it; silence is not new positive input. The current first-header-byte wait has no idle timeout: do not claim it has one or introduce business expiry as part of arbitration. MIXED still releases a candidate at its own deadline.

**TTY:** establish `Disconnected / Connecting / Connected` behavior using nonblocking sockets, `poll(0)`/`SO_ERROR` completion checks, and bounded retry backoff. Treat AF_UNIX EAGAIN/failure deliberately rather than assuming a descriptor means connected. Bound bytes and system calls per tick, cap completed-match counts, and diagnose overflow. A partial command may return OnHold; a completed command returns Active first. Preserve legitimate cross-packet commands without a default 200 ms timeout; clear partial/completed state on disconnect or gate closure. Use RAII buffers and test overlapping prefixes such as command `AAB` in input `AAAB`.

**GPIO:** validate the callback needed by the selected signal mode. Return Active/Inactive using the available level/edge information; do not infer an intermediate debounce state. Edges are observed/consumed events, not a lossless queue. A captured HIGH candidate remains historical even if the level becomes LOW before dispatch. Sustained matching levels can trigger repeatedly.

**Timer and HTTP:** immediate Active/Inactive. Advance Timer on evaluation even when its activation loses arbitration; consume HTTP requests on evaluation as well. Neither input is rolled back merely because a different rule wins.

### 5.3 MIXED algorithm

Sort/build the child list once using the explicit rank table. Retain at most one `PendingCandidate { rule_index, owner, steady_deadline }`; smaller index means higher priority. Do not sort or build an unbounded queue each tick.

1. **Evaluate:** collect all child results before choosing an output.
2. **Capture:** find this tick's highest-priority Active. If no candidate exists, store it and set `deadline = steady_now + 500 ms`. If one exists, replace it only for a strictly higher-priority Active, preserving the original deadline. Equal priority preserves the earliest owner; lower-priority activations are consumed/coalesced.
3. **Resolve:**
   - Without a candidate, return OnHold if any child is holding, otherwise Inactive. Do not create a deadline or fabricate an owner.
   - With candidate rank `k`, defer only if a current OnHold exists at rank `< k` and `steady_now < deadline`.
   - Otherwise detach the candidate, prepend MIXED exactly once, clear pending state, and return Active.

Consequences to preserve in tests:

- Evaluate first at 500 ms; a newly confirmed higher-priority result on that tick can still win. Neither replacement nor repeated same-priority input extends the deadline.
- A higher-priority rule becoming Inactive releases the lower candidate immediately; a lower-priority hold does not delay it.
- A low-priority event released at 500 ms and a high-priority confirmation at 510 ms can produce two separate RULE events. This is not cross-tick deduplication.
- No child Active means no event, regardless of how long another child stays OnHold.
- Captured pulses can be overwritten; full evaluation is not a promise of edge preservation or low-priority attribution fairness.
- Gate closure or rule replacement cancels pending. Storage busy or media pause does not freeze arbitration, because rule reporting is not a file action.

### 5.4 HTTP latch

Support both standalone HTTP and HTTP inside MIXED. The request thread owns only capacity-one submission, not the rule tree's mutable arbitration state. Store the accepted request time for attribution; the evaluation thread consumes it.

Validate request-body size before JSON parsing. Admission uses short critical sections or nonblocking acquisition; contention returns EBUSY promptly. Do not wait for configuration fsync, rule-resource reconstruction, or media drain. Capacity bounds alone do not bound request latency.

Success means accepted for evaluation, not selected as winner, played, or written. Busy latch returns EBUSY; closed rule/schedule gates or missing HTTP selection return the applicable permission/state error. Remove storage prechecks: rule-only, all-actions-disabled, and sound-only requests may still produce RULE. A latched HTTP request may be coalesced by a higher-priority MIXED owner.

## 6. Event model and reporting contract

### 6.1 Trace and attribution

Add a fixed-capacity trace to `EventOwner`. Leaves start with their own type; selected parents prepend before publication. Require `type == trace[0]`, legal depth at most three, and capacity four. A checked prepend must not silently drop the leaf if capacity is exceeded.

Trace describes **only the winning path**, not evaluated siblings, OnHold causes, discarded candidates, action admission, or action results. Capture `uid`, timestamp, condition ID, and source once. Waiting/replacement must not rewrite the retained owner's timestamp; published owners are read-only. Discarded UIDs are not reused.

In the table, `source.id` means the public C event's `source_id`; no additional event container is required.

| Origin | Winning trace (without optional MIXED prefix) | `id` | `source.id` | Delivery when source is present |
| :--- | :--- | :--- | :--- | :--- |
| Inference member | `[INFERENCE_SET, INFERENCE]` | Configured inference `sID` | Input source ID | FRAME |
| Direct EVENT | `[UNKNOWN]` (never wrapped in MIXED) | Empty | Input source ID | EVENT |
| SED | `[SED]` | Configured model/head `sID` | Empty | No source |
| GPIO | `[GPIO]` | Configured signal, such as RISING | Resolved pin name | EVENT |
| TTY | `[TTY]` | Configured command | TTY name | EVENT |
| Timer | `[TIMER]` | Empty | Empty | No source |
| HTTP | `[HTTP]` | Empty | Empty | No source |
| Legacy ALWAYS_ON | `[ALWAYS_ON]` | Empty | Empty | No source |

Examples of complete winning paths are `[MIXED, INFERENCE_SET, INFERENCE]` and `[MIXED, GPIO]`. SCHEDULE remains a gate, not an emitted winning parent.

Use steady time for arbitration, intervals, and wait budgets. FRAME/EVENT owners keep input event time; other owners use the time observed/received, not a later action-completion time. Current wall time remains independent. FILE timestamp is actual publication time. Preserve exact 64-bit UIDs across module-owned JSON output, including values above `2^53`; do not route integers through floating point. Existing UID uniqueness is process-local, not a cross-restart/global identity guarantee.

### 6.2 RULE and FILE behavior

- Each permitted dispatch independently updates `last_rule_owner` and attempts `on_rule_event_cb`, even if all actions are disabled, busy, paused, or unavailable.
- `writer_created = photo_accepted || video_accepted`. It means at least one file task was accepted, not a file was created, not successful completion, and not whether Sound played.
- File tasks carry the exact admitted owner UID and settings. Photo and Video may both publish FILE callbacks for the same RULE; zero successful files is also valid.
- Continuous files use `event_uid=0`, with unique file identity; no synthetic RULE with UID zero is created. Never use event UID alone as a file identifier.
- There is no expected-file-count or all-actions-completed signal. Do not add an event `actions[]` array to infer one.
- Callbacks are best-effort notifications, not a durable ordered stream. FILE may be observed before RULE due to concurrent execution. Do not hold internal locks across notification callbacks to force order.
- Rule info exposes the last **dispatched** owner, including `lTrace`, independently of current/last file tasks. Expose Photo/Video readiness, work/cooldown state, and Sound busy/unavailable state separately. If old aggregate fields remain, define their limited compatibility meaning rather than presenting one slot as all three channels.

## 7. Action execution

### 7.1 Settings and independent admission

Use an immutable `ActionSettings` snapshot containing rule/continuous enablement, action enablement, Photo settings, triggered Video settings, continuous Video settings, and effective watermarks. Continuous settings are MP4 with zero extra interval. Keep revisions and runtime states separate from persisted business fields.

The global revision detects configuration-commit conflicts. It must not terminate all writers for any change. Channel settings/generations invalidate only related pending tasks; an incompatible in-flight media change requests safe finalization. A Photo interval or Sound switch update must not interrupt an unrelated Video task. Media-parameter refresh updates triggered and continuous Video settings coherently.

After attribution and lock release, `dispatchRuleEvent(owner)` observes a consistent configuration/gate state and independently attempts the three channels. Photo/Video require a usable Slot, appropriate media availability, and an idle/cooldown-ready channel; Video additionally requires continuous to be unconfigured. Sound requires no storage. A later failure in one channel does not roll back another accepted channel.

Admission and gate closure must share a linearization/check mechanism, so a result from an old tick cannot reopen a closed channel. Workers recheck admission and resource identity before starting. Do not use a general unbounded queue: each channel has at most one accepted pending-or-running automatic task.

### 7.2 File task lifecycle and parallel workers

Use identity-aware operations such as `tryAccept`, `claim`, `complete(expected_task)`, and `cancelPending`. An accepted task binds owner, immutable settings, and Slot identity. Cancellation of a pending task differs from a request to stop a running task. Completion of old task A must never reset newer task B.

The normal sequence is:

`claim -> admission recheck -> create/write -> finish -> publish -> terminal result -> release -> cooldown`

- Successful completion includes sink finish and non-overwriting final publication. Failed create/write/finish/rename must not emit a success FILE. Define how a usable interrupted clip is retained/reported without labeling it an ordinary completed task.
- Finalize explicitly, release device resources and accounting, then wait for the channel interval. Do not keep a writer alive through cooldown and depend on its destructor to publish. Destructors are exception-safe cleanup fallbacks only.
- Constructor failures, runtime errors, and finalization failures all retire the expected task, balance resources/budgets, and enter bounded per-channel failure backoff. Zero interval must not turn failure into a hot retry loop.
- Create one Photo worker and one Video worker per enabled Slot only after the single-writer lifecycle is safe. Each is serialized within its own channel; they can progress independently when callbacks/resources permit.
- Photo calls JPG/RAW capture and publishes without consuming `slot_media_buffer`. It still participates in media pause and resource admission.
- Only the automatic Video worker consumes `slot_media_buffer`; manual capture keeps its separate buffer and lifecycle. Do not claim unlimited sink capacity or hardware-level parallelism merely because two threads exist.
- When automatic video and manual video coexist, callback sink-capacity errors must be handled as real failures. Every nonzero status, including positive EBUSY, is an error; finish/error paths must release acquired resources under the callback contract.

### 7.3 Sound executor and callback

Append the optional callback `vg_sta_e (*sound_play_cb)(void *user_data)` to `vg_config_media_t`. The module default is null/unavailable. A configured action without a callback must not be reported as accepted playback; expose the unavailable condition or a clear diagnostic. Return zero for success and nonzero for failure using the established callback status convention.

Implement one lightweight `SoundActionExecutor`, owned within the runtime/Context:

- State is `Idle / Pending / Playing / Stopping` under one mutex and condition variable.
- `tryTrigger` checks settings, gate/pause/stop, callback availability, and Idle in a short critical section. Only Idle becomes Pending. Pending/Playing retriggers are rejected, not queued.
- The worker claims Pending, enters Playing, releases the lock, and invokes the callback. All success/error paths restore the appropriate state and notify waiters. Errors affect Sound only and do not replay the trigger.
- Disabling Sound, closing the trigger gate, or stopping cancels Pending. Playing finishes through the callback's bounded completion contract; only then may the worker exit or accept another trigger.
- Explicit `stopAndJoin` precedes destruction. Never join while holding the executor mutex, detach the worker, or forcibly terminate it. Stop and acceptance must be linearized.

The callback contract requires **bounded total completion**, including its cleanup, and valid callback data until the worker joins. No cancellation token is introduced in this signature. A timer/stop flag inside Vigil cannot safely interrupt an arbitrary blocking C function. If the callback cannot meet this contract, keep the capability unavailable rather than claiming bounded shutdown. The executor serializes only its own requests; it does not by itself guarantee ownership of resources used by other callers.

## 8. Gates, continuous video, and lifetime safety

### 8.1 Gate matrix

| Condition | Configured rules and direct EVENT | New triggered actions | Independent continuous Video |
| :--- | :--- | :--- | :--- |
| `bRuleEnabled=false` | No new dispatch. | Reject and cancel pending. | Unaffected by this flag. |
| Schedule closed | No new dispatch. | Reject and cancel pending. | No new clip; request current clip finalization. |
| Continuous configured | Evaluate/report normally. | Photo/Sound normal; triggered Video suppressed. | Run when schedule/storage/media allow. |
| Storage unavailable or insufficient | Evaluate/report normally. | Reject file actions only; Sound remains eligible. | No new clip; finalize/stop as required by failure severity. |
| Media paused | Rule reporting may continue. | Close affected media-operation admission. | Stop/finalize without retaining old device resources across reconfiguration. |
| Service stopping | No new dispatch. | Close all admission and cancel pending. | Stop/finalize and join. |

On rule or schedule closure, clear MIXED pending, the HTTP latch, SED activation/unfinished hold, TTY completed matches/partial command, and cached GPIO edges. Drain/discard disabled-period background input rather than replaying it on reopening. If clearing a backlog requires disconnecting an input, document the reconnection gap rather than claiming lossless edges.

Inference counters survive closure but do not accumulate while closed; hold eligibility is cleared and does not restart merely from a residual counter. Queue-only reset semantics stay unchanged. Timer does not replay every missed interval; reopening can yield at most one currently due activation. GPIO level uses the current value. Configuration replacement clears pending and rebuilds instance state.

An already-started triggered video retains the existing admission-only schedule behavior and may finish after schedule closes. Disabling the rule system requests that triggered video stop. Already-started Photo/Sound callbacks finish within their bounded contract. Continuous video is different: schedule closure is a running-stop condition.

### 8.2 One automatic Video worker, two modes

- Triggered mode consumes admitted owner-bearing Video tasks and obeys its own interval.
- Continuous mode is independently enabled, schedule-gated, and has no owner or extra interval. `bRuleEnabled` is not its continuation condition.
- Enabling continuous immediately stops triggered admission, cancels pending triggered Video, and asks an active triggered clip to finalize with its original UID. Release that sink before creating a continuous one.
- Disabling continuous finalizes its UID-zero clip before permitting the next triggered task to start. Do not resurrect a pending task canceled during a previous mode.
- At schedule closure, request continuous finalization at the next bounded check; do not wait out 60 seconds or wait indefinitely for a keyframe. Reopening starts from a post-admission keyframe without recording the closed interval's backlog.
- Continuous entry/resume drops historical pre-roll. Normal adjacent continuous clips retain the boundary keyframe for the next clip rather than clearing the queue every time. Triggered clips retain their established pre-roll behavior.
- Bound waits for first keyframe, post-start input loss, and ending keyframe. Define interrupted/partial-file and failure outcomes with tests. Mode transitions, pause, and shutdown must break these waits.
- Preserve the 60-second target and keyframe-aware slicing; exact media duration and gap-free recording are not guaranteed. Synchronous finish/thumbnail work can exceed the approximately 10-second media buffer. Measure gaps and memory before considering a separate bounded post-processing mechanism.

### 8.3 Media-operation barrier and shutdown

Fix the barrier before introducing parallel execution:

1. Pause check and active-operation admission occur in the same critical section. Acquire an exception-safe operation guard **before** a fallible create call, so failed construction cannot leak the active count.
2. Cover automatic/manual video create/write/finish, image capture, device-dependent thumbnails, and Sound. Pure filesystem publication may finish after media is paused, but before storage/Context teardown.
3. Define balanced overlapping suspend/resume behavior so one resume cannot reopen media while another suspension remains. Waiting suspension closes admission before draining already-admitted operations.
4. Do not hold global instance, configuration, executor, or callback-dependent resource locks while waiting for those workers. Obtain a safe runtime reference and release the C API entrance lock before a potentially reentrant join/drain operation.
5. Stop in explicit dependency order: close service admission and stop the rule producer; cancel pending; stop/drain/join Sound, manual capture, and both automatic file workers; finish publication; then stop storage and release Context/runtime state. Do not rely on member-destruction order to stop producers early enough.
6. Keep callback pointers and `user_data` valid until all workers have joined. Notification callbacks must return promptly and must not synchronously call deinit, eject their own Slot, or another self-join operation.
7. Start media admission closed when callback resources are not yet ready; open it only through the documented readiness/resume path. Partial initialization must unwind every started thread and acquired resource.

These guarantees are conditional on bounded callbacks and filesystem/device operations. Tests with fakes establish Vigil's synchronization, not the interruptibility of arbitrary drivers.

## 9. Storage and resource coordination

- Estimate simultaneous demand as at most one automatic video plus a photo, including pre-roll, GOP tail, thumbnails, and safety margin. Triggered and continuous Video are mutually exclusive, so do not budget them as two automatic videos.
- Watermarks are estimates, not reservations or capacity guarantees. `.cache` in-flight files are not fully represented in the index. Register lightweight in-flight budgets under a short shared admission critical section, including manual capture on the same filesystem, and release them on every terminal path.
- Continue checking actual write errors and ENOSPC. Distinguish ordinary new-task headroom rejection from critical/device failure that requires stopping an active writer. Raising Photo's admission watermark must not automatically terminate a healthy video.
- Replace destructive directory probing with idempotent creation. In particular, do not use `is_directory=false -> remove_all` against a path another publisher may be creating.
- Coordinate publication/rename with rotation, free-up, and explicit removals so a destination directory cannot disappear during publication. Existing index locks alone do not protect the entire filesystem sequence. Do not run expensive thumbnails or external callbacks under the coordination lock.
- Synchronize space-probe caches, counters, index updates, and shared state accessed by both workers. Keep tests around midnight directory creation and date-directory removal.
- Use collision-resistant automatic names with task/sequence identity and a no-overwrite publication policy. Timestamp plus extension is insufficient; UID-zero continuous clips still need unique identities, including collision handling after restart.
- On Slot disable, switch, or removal, close admission and drain/join both old consumers before opening the replacement. Tasks retain their original Slot and never move to another device. If the second worker fails to start, stop/join the first and unwind the partial enable operation.

## 10. C API changes and compatibility boundary

In `include/vigil_ctypes.h`:

- Append `VG_RULE_TYPE_MIXED = 10`; retain existing enum values 0-9.
- Define `VG_RULE_TRACE_MAX_DEPTH` as 4. Append `vg_rule_type_e trace[VG_RULE_TRACE_MAX_DEPTH]` and `size_t trace_len` to `vg_rule_evt_t`, preserving existing fields and explaining their revised semantics.
- Append `sound_play_cb` to `vg_config_media_t` with the signature and bounded contract in section 7.3.
- Update `VG_CTYPES_VERSION` and the relevant `VG_CAPI_VERSION` declaration/comments in `include/vigil_capi.h` to reflect the changed public contract. Update every module-owned enum/string conversion and initializer.

This is a **C ABI change**. Even tail-appending to `vg_config_media_t` shifts later fields in `vg_config_t`; the current initialization API has no size/version negotiation. The library and callers built with the new layout must be rebuilt together. Do not claim that header version strings or tail-appending make old binaries compatible. Audit zero initialization, aggregate initialization, and field-by-field initialization in module examples and tests.

New emitted events have `1 <= trace_len <= 4` and `type == trace[0]`. At module-owned C/JSON boundaries, validate lengths, enum values, and the first type before reading/copying. Zero trace length may fall back to `[type]` for source migration under the **new** layout; invalid data is diagnosed and safely rejected or normalized to one validated type, never read out of bounds. This fallback does not repair an ABI mismatch.

Strings/event objects are borrowed for the duration of the callback; consumers retaining them must copy them. Copy trace elements, do not retain stack-backed pointers. Do not add an assumption that all callbacks run on one thread. The module owns C callbacks and its HTTP JSON; do not add a separate notification transport merely to test the trace contract.

## 11. Six auditable implementation phases

Each row is a review unit, not a mandate to combine unrelated edits into one large commit. Split further when needed. Every unit must build and have relevant tests; keep unsupported capabilities explicitly unavailable until their end-to-end module path works. Use legacy adapters for intermediate compatibility, not successful no-op configuration writes.

### Phase 1: Reproducible tests and protocol/configuration protection

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 1A | `CMakeLists.txt`, new module-owned `tests/` | Add optional, reproducible CTest coverage and minimal fake-clock/callback support. Characterize current single-rule, input-reset, event, and legacy behavior. Do not rely on generated test artifacts. |
| 1B | `include/`, `vigil/types.hpp`, module event projection, `example/` | Add enum/trace and optional null Sound callback, version declarations, and C/C++ initializer tests. Preserve old-type reporting; test bounds, source fields, and exact UIDs. Playback/MIXED remain unavailable until implemented. |
| 1C | `vigil/configs.hpp`, `vigil/endpoints.hpp` | Add schema recognition, canonical legacy-equivalent saving, safe commit/startup foundations, future-version protection, and destructive-write rejection. Do not prematurely enable unsupported multi-action/continuous payloads. |

### Phase 2: Repair the current execution foundation before splitting workers

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 2A | `vigil/context.hpp`, `vigil/writers.hpp`, `vigil/slot.hpp` | Identity-aware tasks, exception-safe accounting, explicit finish/publish before cooldown, uniform failure cleanup/backoff. Test old-A/new-B completion and all callback/publication failures with one writer. |
| 2B | `vigil/capi.cpp`, `vigil/vigil.hpp`, media-operation guards | Linearize pause/admission, cover images/thumbnails/manual capture, and fix runtime/worker stop order. Test reentry, overlapping suspension, drain, and partial initialization. |
| 2C | `vigil/slot.hpp`, `vigil/index_manager.hpp` | Non-destructive directory creation, unique publication, synchronized probes, and publication/removal coordination. Test collisions, concurrent creation, and Slot removal without adding parallel automatic writers yet. |

### Phase 3: Full evaluation, three-state results, MIXED, and HTTP/EVENT

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 3A | `vigil/types.hpp`, `vigil/rules.hpp`, `vigil/context.hpp` | EvalResult, leaf attribution, independent clocks, and full `InferenceRuleSet` evaluation. Test slow/unrelated/negative/missing inputs, EVENT non-decay, queue reset, and no empty-tick duplicate activation. |
| 3B | `vigil/rules.hpp` and focused input tests | Bound TTY connect/read work, fix prefix ownership/matching, expose consistent SED state, and validate GPIO callback/consumption behavior. Resolve documented edge cases in isolated changes. |
| 3C | `vigil/configs.hpp`, `vigil/rules.hpp`, `vigil/endpoints.hpp` | MIXED config/rank/pending state, bounded HTTP latch, dual EVENT/configured dispatch, gate cleanup, and safe rule-resource handover. Validate 499/500/501 ms and first-wins/replacement. Keep the existing single-file executor adapter. |

### Phase 4: Immutable action settings and bounded Sound execution

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 4A | `vigil/configs.hpp`, `vigil/context.hpp`, writer settings adapters | Extend the commit foundation to ActionSettings and per-channel revisions. Preserve the usable legacy single-writer API; test config/refresh conflicts and partial-durability outcomes. |
| 4B | Runtime-owned Sound executor, C callback fakes | Implement Idle/Pending/Playing/Stopping with pause/stop integration. Test null/error callbacks, Pending/Playing rejection, cancel-before-start, bounded in-flight completion, and no retry/backlog. Do not expose a nonfunctional action API. |

### Phase 5: Two file channels and a usable multi-action API

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 5A | `vigil/context.hpp`, `vigil/writers.hpp`, `vigil/slot.hpp`, legacy adapter | Independent Photo/Video tasks, workers, budgets, and per-channel state. Preserve manual capture and legacy ALWAYS_ON before expanding the public action shape. Test identity/settings isolation, simultaneous work, and partial worker startup. |
| 5B | `vigil/configs.hpp`, `vigil/endpoints.hpp`, dispatch/info | Enable version-2 multi-action configuration and canonical round trips. Validate both POSTs and startup against ALWAYS_ON restrictions. Exercise Photo+Video+Sound and same-UID multiple FILE callbacks with fakes. Sound-only works without storage; last RULE is independent. Still reject `continuous=true`. |

### Phase 6: Schedule-aware independent continuous video and final review

| Unit | Scope | Expected result and acceptance |
| :--- | :--- | :--- |
| 6A | Video worker, clocks/gates, action configuration | Enable continuous recording. Test `rule=false` with schedule open, schedule closure finalization, bounded keyframe/input-loss waits, UID-safe mode handover, and no stale pre-roll or overlapping automatic sinks. |
| 6B | Module docs, public comments/examples, complete tests | Regress legacy MP4/JPG/RAW and intervals, finish API/docs updates, run standalone/sanitizer checks, and report limitations and unverified callback capabilities. Update the TODO checklist with evidence. |

Small commits make review and rollback analysis easier, but public layout and persisted schema still have compatibility dependencies. Do not suggest swapping in an old library against new structs or silently down-converting complex configurations. Keep versioned fixtures and document which old configurations remain representable.

## 12. Deterministic validation matrix

Tests must execute production rule, configuration, executor, and writer code. Do not copy the arbitration algorithm into a test and only test that copy. Use a controllable clock, scripted inputs, fake media callbacks, temporary filesystems, and explicit thread barriers. Avoid random sleeps as correctness evidence.

| Group | Required scenarios | Key assertions |
| :--- | :--- | :--- |
| R1 Full evaluation | Earlier child continuously Active; later child positive/negative; one child throws; consecutive direct EVENT inputs. | Every applicable child advances once per tick; EVENT is not a negative FRAME; output attribution remains deterministic. |
| R2 Debounce/hold | 960 ms input spacing, missing/unrelated input, negative residual count, reset, close/reopen, confirmed empty tick. | Only matching FRAME changes the counter; no arbitrary staleness reset; hold eligibility is separate; no repeated empty-tick owner. |
| R3 Candidate selection | Simultaneous Active, repeated equal priority, higher replacement, lower additions, only OnHold. | Earliest equal-priority owner; fixed deadline; accepted lossiness; no owner fabricated without Active. |
| R4 Time boundaries | 499/500/501 ms; higher confirmation on the deadline and at 510 ms; wall-clock rollback. | Evaluate before expiry; no deadline extension; late confirmation may create another RULE; steady budgets remain valid. |
| R5 Peripherals | GPIO HIGH then LOW, edge bursts; TTY fragmented/overlapping commands, continuous bytes, retries; SED silence/disconnect/confirmation/ignored packets. | Historical candidate retention, bounded work, no fake edge-losslessness, consistent hold/activation snapshots. |
| R6 HTTP/EVENT | Concurrent activation/eval/config fsync/handover, oversized body, busy latch, two outputs on one tick, missing storage. | Prompt contention errors; asynchronous ACK meaning; two RULE callbacks; no storage gate on rule-only/Sound. |
| A1 Independent actions | Video busy while Photo/Sound trigger; all actions off; Sound-only; continuous configured. | Independent admission; permitted RULE still reports; `writer_created` is file acceptance only. |
| A2 Task identity | A running, gate closes/reopens, B admitted, A completes; Slot replacement. | A cannot clear/restart B or migrate owner/settings/device identity; canceled pending work never starts. |
| A3 Completion/errors | Zero/short/long interval; create/write/finish/rename failures; nonzero positive callback statuses. | Publish before cooldown; correct terminal result; no false FILE success; balanced guards/budgets and bounded backoff. |
| A4 Settings | Photo/Sound-only changes, Video refresh, admission watermark increase. | Only related channels invalidated; both video-mode settings refreshed; healthy work not stopped solely by new admission headroom. |
| A5 Pause/stop | Admission racing pause; image/thumbnail/manual video/Sound in flight; nested pause; callback reentry; partial worker startup. | No old-resource operation after drain; no lock cycle, self-join, leaked activity, orphan worker, or dangling callback data. |
| A6 Continuous | Rule disabled, schedule boundaries, both mode switches, missing first/last keyframe, input loss, manual capture overlap. | One automatic sink, bounded stop, correct UID, no stale pre-roll, correct boundary-frame/PTS handling. |
| A7 Storage | Midnight directory race, rotation/removal during publish, naming collision, ENOSPC, Slot removal, same-filesystem manual capture. | No destructive create/overwrite, released reservations, no false publication success, safe device lifetime. |
| S1 Sound states | Null callback, callback error, Pending/Playing retrigger, disable/stop before and during execution. | No backlog or business cooldown; no retry of a failed trigger; acceptance/stop linearized; join waits only for bounded admitted work. |
| C1 Schema | Legacy/new/mixed/future versions, missing fields, wrong types, fractions/overflow, inactive blocks, destructive old writes. | Canonical GET/save; precise rejection; no unsupported-version rewrite or loss of advanced configuration. |
| C2 Commit/replacement | Revision conflict, pre-rename failure, post-rename directory-fsync failure, construction/reacquisition/restoration failure. | Observable visibility/durability boundary; no partial flags or misleading rollback; cleanup and fail-closed restoration. |
| C3 Startup | Missing files, corrupt rule/action/schedule, saved MIXED+SED restart, unimplemented capability in an intermediate phase. | Safe defaults, retained invalid files, no default ungated/ALWAYS_ON execution, no success-without-behavior. |
| C4 ALWAYS_ON | MP4/JPG/RAW, enable/schedule combinations, intervals, both POST orders and concurrent POSTs. | No automatic continuous conversion or format/pacing change; single-file-action invariant cannot be bypassed. |
| W1 Event boundary | All enum types, single/MIXED/direct EVENT trace, zero/MAX/oversized lengths, null/empty source, UID above `2^53`. | Valid bounded copying, top-type consistency, normalized attribution, exact integers, no trigger/action arrays. |
| W2 File association | Photo+Video from one owner, failures with zero successful files, FILE before RULE, several continuous UID-zero clips. | Task-bound UID remains stable; no promised count/order; file identity distinct from event UID. |

### 12.1 Reproducible module-only build and test setup

- The source build requires CMake 3.21+, a C++20 compiler, Protobuf/protoc, PkgConfig, libblkid, and nlohmann/json. Protobuf is required even when `VIGIL_USE_SYSTEM_PROTOBUF` is off; there is no bundled Protobuf build.
- `VIGIL_USE_SYSTEM_NLOHMANN_JSON` defaults off; using the system package requires the version specified in `CMakeLists.txt` (currently 3.12.0). Obtain the declared `3rdparty/` dependencies before offline validation.
- Jemalloc currently defaults on and replaces global C++ allocation. Bundled mode needs make/autoconf; keep dependency and allocator changes out of this refactor unless necessary and separately reviewed.
- Use a **fresh** standalone build directory such as `build-refactor-host/`, with `VIGIL_USE_BUILDROOT_SYSROOT=OFF`, rather than editing the existing `build/` cache. Record the selected compiler, dependency sources, and allocator options. Standalone inference generation uses `protocol/inference.proto`.
- Source-defined targets are `vigil`, plus `vigil_example`, `vigil_index_manager_test`, and `vigil_mask_test` when `VIGIL_BUILD_EXAMPLES=ON`. Building them checks compilation; do not automatically run interactive/privileged examples as tests.
- Phase 1A must add the missing first-party tests and CTest registration, preferably behind a module-specific opt-in such as `VIGIL_BUILD_TESTS`. This option is **proposed, not currently implemented**. A fresh CTest listing must contain the intended cases; zero discovered tests is not success.
- Once that option exists, a representative host configuration is `cmake -S . -B build-refactor-host -DVIGIL_USE_BUILDROOT_SYSROOT=OFF -DVIGIL_BUILD_EXAMPLES=ON -DVIGIL_BUILD_TESTS=ON -DVIGIL_ENABLE_JEMALLOC=OFF`; follow it with `cmake --build build-refactor-host` and `ctest --test-dir build-refactor-host --output-on-failure`. This deliberately selects an allocator-off logic-test baseline; validate the supported normal allocator configuration separately. These are future validation instructions, not commands run for this handoff.
- Run ASan/UBSan and TSan in separate supported configurations, documenting allocator/sanitizer limitations rather than hiding failures. Ordinary unit tests should need neither real peripherals nor elevated filesystem-monitor privileges.
- Distinguish host logic correctness from target timing/media capability. Report actual evaluation delay, callback bounds, memory/CPU use, and clip gaps only when measured. Do not claim hardware success, hard real-time bounds, or gap-free recording from fake callbacks.

## 13. Review gates and return package

Before implementation, fix test constants for arbitration, TTY read/backoff budgets, finite media waits, and per-channel failure backoff. The initial 500 ms value is agreed as a starting arbitration budget, not a new source validity definition. Select additional limits deliberately, test their boundaries, and record them without expanding configuration unnecessarily.

Before enabling a phase's capability, its prerequisite task ownership, startup/commit safeguards, and callback availability must be in place. Do not advertise Sound as safely stoppable without a bounded callback contract. Do not enable multiple file tasks before independent channel state, shared filesystem coordination, and identity-aware finalization exist.

Return the refactored module with:

1. A concise, ordered change/commit summary mapped to the six phases and TODO identifiers.
2. Updated local API/architecture documentation, version declarations, legacy/new configuration fixtures, and module examples.
3. Reproducible build/test commands and results, including test discovery, failure-injection coverage, sanitizer limitations, and explicitly unrun validation.
4. A list of contract deviations, unresolved compatibility edge cases, and callback/performance limits needing review.
5. An updated `docs/TODO.md` with evidence for completed items and explicit blockers for unfinished work.

Keep the return package module-local. This document is a handoff specification, not an authorization to expand the implementation beyond Vigil.
