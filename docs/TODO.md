We plan to refactor to meet new requirements. First, please familiarize yourself with the project structure, then design, discuss (ask promptly if anything is unclear), and refine the refactoring plan. The specific details are as follows:

**Engineering Guidelines (Plan First):**

1. Break the work down into multiple auditable steps. Keep the changes in each step as small and independent as possible to facilitate code review, allow easy rollbacks, and minimize risk.
2. Clearly define the goals and expected outcomes for each step, ensuring every change has a clear rationale and verification method. Aim for minimal viable changes to avoid bloat and over-engineering.
3. Pay attention to layered design by clearly defining the responsibilities and boundaries of each layer. During discussions, proceed top-down from high-level architecture to low-level details.
4. Maintain code maintainability and extensibility by following software engineering best practices. The constraints listed here may not be exhaustive and should be continuously supplemented and refined during actual development.

**Functional Requirements (Decoupling Rule Evaluation from Post-Trigger Actions):**

1. Add a `Mixed Rule` type to `Record Rule` to support multiple distinct trigger conditions simultaneously (at most one rule per type, with multiple rules allowed across different types). Evaluation latency and performance must be taken into account.
2. Post-trigger actions are categorized into: taking photos (JPG, RAW), recording videos (MP4), and playing audio (it is recommended to add a new C API interface and implement audio playback at the C layer). Multiple actions must be configurable for parallel execution, which requires proper abstraction here.
3. Support configurable standalone continuous recording. When standalone recording is enabled, the video recording action within the post-trigger behaviors should automatically be disabled.
4. Enhance the content of post-trigger event reporting. For complex `Record Rules`, structured event information may need to be retained to ensure observability.

# DRAFT

## 1. Scope and engineering discipline

- [ ] **ENG-1** Separate configuration, rule evaluation, action execution, and the public C API. Keep each layer's responsibilities explicit.
- [ ] **ENG-2** Implement the six phases in `docs/PLAN.md` as small, independently reviewable changes. Keep prerequisite correctness fixes separate from new rule/action semantics.
- [ ] **ENG-3** Preserve unrelated APIs and behavior. Do not introduce a generic scheduler, thread pool, multi-candidate queue, durable event bus, or source-identity framework.
- [ ] **ENG-4** Keep implementation, tests, examples, and documentation within this module. Represent media dependencies through the existing callback boundary and deterministic fakes, not device-specific implementations.
- [ ] **ENG-5** Establish reproducible module-owned tests and CTest registration. Existing examples and generated files in `build/` are not a regression suite.
- [ ] **ENG-6** Every change must have an objective, expected result, compatibility implications, and validation evidence. Never accept a configuration for a capability that the intermediate implementation does not yet support.

## 2. Rules, full evaluation, and arbitration

- [ ] **RULE-1** Add `MIXED`: OR across distinct rule types, with at most one child of each type. Allow only `INFERENCE_SET`, `SED`, `GPIO`, `TTY`, `TIMER`, and `HTTP`; reject duplicates, empty lists, unknown types, and nested `MIXED`.
- [ ] **RULE-2** Evaluate every enabled, schedule-open MIXED child exactly once per tick. Evaluate every `InferenceRuleSet` member even after an earlier member matches. A recoverable child error must not skip later children.
- [ ] **RULE-3** Use `Inactive`, `OnHold`, and `Active` results. Only `Active` carries an owner. Keep full state advancement separate from winner selection; `OnHold` is an unfinished-confirmation qualifier, not source health or a new Boolean algebra.
- [ ] **RULE-4** Apply the explicit priority order `INFERENCE_SET > SED > GPIO > TTY > TIMER > HTTP`, independent of configuration-list order and existing tied precedence values.
- [ ] **RULE-5** Maintain one pending MIXED candidate: earliest owner wins at equal priority; strictly higher priority replaces the owner without refreshing the deadline. Lower-priority matches are consumed and discarded/coalesced.
- [ ] **RULE-6** Wait only for higher-priority `OnHold` results, using an initial 500 ms arbitration budget. Evaluate before deciding on the deadline tick. Never create an event from timeout alone or reset inference debounce because the deadline elapsed.
- [ ] **RULE-7** Explicitly retain single-winner, lossy coalescing. Do not claim lossless edges, fair attribution, cross-tick scene deduplication, or one report per physical incident. A late confirmation may produce another RULE.
- [ ] **RULE-8** Preserve inference source filters, confidence/class filters, ROI behavior, and increment/decrement debounce. Missing frames, unrelated sources, and discrete EVENT input must not alter the counter. Queue reset remains queue-only.
- [ ] **RULE-9** Track inference hold eligibility separately from its counter: negative input or gate closure ends eligibility; an empty tick after confirmation is `Inactive`, not a repeated owner or renewed hold.
- [ ] **RULE-10** Keep SED input confirmation separate from sound playback. Use one consistent snapshot of pending activation and unfinished confirmation; consume activation first. Preserve the time-window algorithm and document silence, ignored input, disconnect, and gate-close behavior.
- [ ] **RULE-11** Make TTY connection and evaluation bounded: nonblocking connection state, completion checks, retry backoff, per-tick read limits, bounded match counts, RAII buffers, and correct overlapping-prefix matching. Do not add an arbitrary 200 ms partial-command reset.
- [ ] **RULE-12** GPIO remains `Active`/`Inactive` unless the callback actually supplies more state. Validate the callback for the selected level/edge mode. Preserve observed historical candidates and explicitly acknowledge edge coalescing.
- [ ] **RULE-13** Replace direct HTTP dispatch with a thread-safe capacity-one request latch consumed by the evaluation thread. Bound payload size before parsing and reject lock contention quickly; activation must not wait for configuration I/O, resource handover, or media drain.
- [ ] **RULE-14** An HTTP acknowledgement means request acceptance, not arbitration victory or action success. Storage availability must not reject rule-only or sound-only activation.
- [ ] **RULE-15** Preserve direct discrete EVENT delivery, gated by rule enablement and schedule. Also evaluate the configured tree on that tick. Dispatch EVENT first, then the configured winner; both publish separate RULE events and independently attempt action admission.

## 3. Event identity and observability

- [ ] **EVT-1** Extend `EventOwner` and `vg_rule_evt_t` with a bounded top-to-leaf winning `trace`; require `type == trace[0]`. Current legal depth is at most three, with capacity four and explicit overflow checks.
- [ ] **EVT-2** Preserve simple RULE events: no `triggers[]`, no action-result `actions[]`, and no all-children evaluation trace. `dActions` is a configuration object only.
- [ ] **EVT-3** Normalize attribution: inference condition ID/source ID; SED model ID; GPIO signal/pin name; TTY command/port name; empty condition IDs for Timer, HTTP, and direct EVENT. Preserve FRAME/EVENT delivery semantics.
- [ ] **EVT-4** Capture owner UID, timestamp, ID, and source once. Prepend each parent type only once before publication; publish immutable ownership and never reuse a discarded candidate's UID.
- [ ] **EVT-5** Separate event time, current wall time, and steady time. Update clocks even when rules are disabled; inference timestamps must not replace the system's current time. FILE timestamps describe publication.
- [ ] **EVT-6** Preserve exact 64-bit UID values. Every file action retains its admitted owner's UID; several FILE events may share one RULE UID. Continuous files use `event_uid=0` and retain independent file identities.
- [ ] **EVT-7** Define `writer_created` as `photo_accepted || video_accepted`, not successful file creation, sound success, or proof that all actions completed.
- [ ] **EVT-8** Maintain `last_rule_owner` independently of file tasks. Expose `lTrace` and separate Photo/Video/Sound status in rule info; document any retained aggregate compatibility fields.
- [ ] **EVT-9** Document callback borrowing, best-effort reporting, possible RULE/FILE reordering, and zero-to-many file association. Do not promise an expected artifact count or end-to-end delivery guarantee.

## 4. Configuration and compatibility

- [ ] **CFG-1** Introduce schema version 2 for record action and selected-rule configurations. Reject unsupported future versions without modifying their files; distinguish missing files from invalid existing files.
- [ ] **CFG-2** Add `bContinuousRecordEnabled` and `dActions.dPhoto/dVideo/dSound`. New actions default to disabled; Photo defaults to JPG; Photo/Video intervals default to 5000 ms. Sound has only an enable switch.
- [ ] **CFG-3** Validate types strictly. Photo accepts only JPG/RAW, Video is MP4, and Photo/Video intervals are nonnegative representable integers. Reject fractions, negative values, and overflow rather than substituting defaults.
- [ ] **CFG-4** Read legacy `dWriterConfig` as the equivalent single file action, preserving rule enablement, format, and interval. Keep continuous and sound disabled. Do not automatically convert `ALWAYS_ON` to continuous MP4.
- [ ] **CFG-5** Write and return canonical versioned JSON. Reject mixed new/legacy structures and destructive legacy whole-object overwrites of MIXED, multi-action, sound, or continuous configurations. Retain inactive rule configuration blocks.
- [ ] **CFG-6** Commit enablement, actions, derived settings, and watermarks as an immutable snapshot. Serialize configuration commits and media-settings refresh; revision conflicts must not partially apply.
- [ ] **CFG-7** Define durable commit boundaries: failure before rename leaves old disk/runtime state; successful rename publishes the matching runtime snapshot. A later directory-fsync failure means **applied, durability unconfirmed**, not uncommitted. Expose that state and block further persistent commits until recovery.
- [ ] **CFG-8** Validate configuration before enabling automatic work at startup. Corrupt rules, actions, or schedule must fail closed without overwriting the source file or silently enabling default `ALWAYS_ON`/ungated recording.
- [ ] **CFG-9** Prepare replacement rules before releasing old resources where possible. Use explicit handover for exclusive resources, clean partial construction, and fail closed if best-effort restoration fails. Do not promise restoration of old debounce/timer/connection state.
- [ ] **CFG-10** Validate legacy `ALWAYS_ON` restrictions in both configuration POSTs and startup under the same serialized combined-state check: one enabled file channel, sound disabled, continuous disabled. Preserve MP4/JPG/RAW, original pacing, schedule, and UID association.
- [ ] **CFG-11** Resolve existing documented-versus-implemented edge cases with separate tests and compatibility notes: debounce zero, empty SED class filters, interval zero, and direct EVENT attribution. Do not silently redefine them inside the refactor.

## 5. Independent action execution

- [ ] **ACT-1** Dispatch Photo, Video, and Sound independently after attribution. Failure, cooldown, or unavailability in one channel must not suppress the other channels or RULE reporting.
- [ ] **ACT-2** Give Photo and Video independent settings and intervals. The next successful start is no earlier than `max(previous actual start + interval, previous finalization completion)`; zero interval does not permit concurrent writers within one channel.
- [ ] **ACT-3** Bind each accepted file task to its owner, immutable channel settings, and Slot identity. Implement identity-aware `tryAccept`, `claim`, `complete(expected_task)`, and pending cancellation; separate cancellation from stopping running work.
- [ ] **ACT-4** Keep global revision for commits, not universal writer interruption. Channel-specific changes affect that channel; media refresh updates both triggered and continuous video settings coherently.
- [ ] **ACT-5** Use one Photo worker and one automatic Video worker per enabled Slot. Photo must not consume the video queue. Manual capture retains its own queue and lifecycle; do not build a general job scheduler.
- [ ] **ACT-6** Add one sound worker with `Idle / Pending / Playing / Stopping` protected by a mutex and condition variable. Reject Pending/Playing retriggers, keep no backlog or business cooldown, and do not retry a failed trigger automatically.
- [ ] **ACT-7** Add optional `vg_sta_e (*sound_play_cb)(void *user_data)` to the media callback surface. Null means unavailable. Call outside internal locks and report real errors; require bounded total callback completion.
- [ ] **ACT-8** Cancel pending sound on disable/gate closure/stop; let an in-flight bounded callback finish before joining. No detached threads, forced termination, or claims that a stop flag can interrupt an arbitrary C callback.

## 6. Gates and continuous recording

- [ ] **GATE-1** Rule disable and schedule closure block new configured/direct EVENT dispatch and triggered-action admission. Clear MIXED pending, HTTP/SED latches, TTY completed/partial input, and cached GPIO edges; do not replay disabled-period input.
- [ ] **GATE-2** Preserve inference counters across gate closure but stop accumulating and clear hold eligibility. Timer does not catch up every missed interval; GPIO level reads the current value on reopening.
- [ ] **GATE-3** Make gate changes and action admission linearizable so an old tick cannot enqueue work after closure. Never join a worker while holding a configuration lock it needs.
- [ ] **GATE-4** Independent continuous MP4 obeys schedule but ignores `bRuleEnabled`. Storage and media availability still gate its execution. When continuous is configured, triggered Video is suppressed even while continuous cannot currently run; do not rewrite `dVideo.bEnabled`.
- [ ] **GATE-5** Use the single Video worker for triggered/continuous mode handover. Cancel old pending work, finalize the current clip with its original owner, and release its sink before starting the other mode. Never overlap automatic muxers or revive old tasks.
- [ ] **GATE-6** On schedule closure, finalize the current continuous clip promptly through bounded checks. Preserve the existing admission-only schedule policy for already-started triggered video; rule disable requests triggered-video stop.
- [ ] **GATE-7** Continuous entry/resume starts at a new post-admission keyframe without historical pre-roll. Normal adjacent continuous clips retain keyframe continuity. Keep the 60-second target, not an exact-duration or gap-free guarantee.
- [ ] **GATE-8** Bound initial-keyframe wait, end-keyframe wait, and input-loss handling. Define failure/interrupted-clip behavior and avoid zero-interval failure retry loops.

## 7. Lifecycle, media admission, and storage prerequisites

- [ ] **SAFE-1** Fix current single-writer ownership/finalization defects before adding parallel writers. Old task completion must never clear a newer task.
- [ ] **SAFE-2** Linearize media pause with new-operation admission and use exception-safe activity guards before fallible create calls. Cover automatic/manual video, image capture, device-dependent thumbnails, and sound.
- [ ] **SAFE-3** Support balanced overlapping pause/resume and explicit stop ordering. Release global instance/configuration/device-operation locks before waiting for workers that need or may reenter them.
- [ ] **SAFE-4** Execute `claim -> admission recheck -> create/write -> finish -> publish -> terminal result`, release resources, then apply channel cooldown. Destructors are cleanup fallbacks, not the normal publication mechanism.
- [ ] **SAFE-5** Handle create/write/finish/rename failures uniformly: correct terminal status, no false success FILE, balanced activity/budget counts, and bounded per-channel retry backoff.
- [ ] **SAFE-6** Make directory creation idempotent and non-destructive. Coordinate final publication with rotation/removal; use collision-safe non-overwriting automatic file naming, including continuous UID-zero files.
- [ ] **SAFE-7** Synchronize shared space probes and state. Estimate one simultaneous video plus photo, include pre-roll/GOP/thumbnail margin, and register in-flight budgets across automatic and manual capture on the same filesystem. Still handle actual ENOSPC and device failure.
- [ ] **SAFE-8** Do not terminate a healthy active video merely because a settings change raises the new-task admission watermark.
- [ ] **SAFE-9** On Slot switch/removal, close admission and join the old consumers before opening the new Slot. Do not migrate tasks across devices. Roll back partial worker startup safely.
- [ ] **SAFE-10** Stop rule production, cancel pending work, drain/join action and capture workers, finish publication, then tear down storage and Context. Keep callback data alive until joins complete and prohibit synchronous self-join operations from callbacks.

## 8. Public API and module documentation

- [ ] **API-1** Append `VG_RULE_TYPE_MIXED = 10`; preserve values 0-9 and update every module-owned type/string conversion.
- [ ] **API-2** Append fixed-capacity trace fields, validate lengths/types, and update public version declarations. `trace_len=0` fallback is source compatibility for the new layout, not binary compatibility.
- [ ] **API-3** Document that growing `vg_config_media_t` changes enclosing `vg_config_t` layout. Rebuild the library and module examples together; no old/new binary-layout mixing is supported by the current initialization API.
- [ ] **API-4** Audit all module-owned initializers, borrowed-pointer lifetimes, and callback status handling, including nonzero positive error codes and null callbacks.
- [ ] **API-5** Update `docs/minimal-http-api.md`, `docs/architecture.dot`, public header comments, and relevant examples after implementation. Keep current behavior distinct from planned behavior until then.
- [ ] **API-6** Document capability limits without embedding playback, device routing, or application implementations in Vigil. A fake callback passing tests does not establish real-device timing guarantees.

## 9. Verification and review handoff

- [ ] **TEST-1** Test production rule/arbitration code with a controllable clock and scripted inputs: full evaluation, slow inputs, reset, priority ties/replacement, 499/500/501 ms boundaries, and EVENT plus configured RULE output.
- [ ] **TEST-2** Test bounded TTY/SED behavior, GPIO consumption, HTTP concurrency/payload bounds, gate close/reopen, and configuration replacement failures.
- [ ] **TEST-3** Test independent action admission, task identity races, cooldown/finalization ordering, settings isolation, sound no-backlog semantics, pause/stop, and partial initialization.
- [ ] **TEST-4** Test continuous-mode handover, rule-disabled operation, schedule boundaries, no stale pre-roll, input loss, manual capture concurrency, storage rotation/removal, and fault cleanup.
- [ ] **TEST-5** Test legacy/new/invalid schemas, canonical round trips, destructive legacy writes, both POST orders/concurrency, ALWAYS_ON compatibility, startup failure, and persistence failure before/after rename.
- [ ] **TEST-6** Test callback/JSON boundaries where owned by Vigil: all rule types, trace limits, attribution, exact UIDs above `2^53`, multiple FILE callbacks per RULE, and independent continuous files.
- [ ] **TEST-7** Use deterministic thread barriers, fake callbacks, and temporary filesystems. Run ASan/UBSan and TSan separately where supported; do not substitute sleeps or stress-only runs for race tests.
- [ ] **TEST-8** Verify a fresh standalone build and reproducible CTest discovery without modifying existing build caches. Clearly separate build success, host logic tests, sanitizer results, and any unavailable device validation.
- [ ] **TEST-9** Return a module-only review report with commit sequence, changed interfaces/configurations, test commands/results, remaining limitations, and deviations requiring review. Update this checklist with evidence before handing back the module.
