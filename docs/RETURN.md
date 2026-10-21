# Return package: the Vigil refactor

Date: 2026-10-20. Branch `dev`, the 34 commits after `origin/dev` (`c1ad8bf` to the commit that adds this document). This is the module-only review report that `docs/PLAN.md` section 13 asks for; the plan carries the status of every section and unit, and `docs/TODO.md` the evidence for every checklist row. Nothing outside the module (`vigil/`, `include/`, `tests/`, `example/`, `docs/`, the build files) changed.

## 1. Change summary, in commit order

The phases are those of `docs/PLAN.md` section 11; the identifiers are the rows of `docs/TODO.md`. One or two commits per unit, plus the documentation and review-fix commits; the correctness fixes of Phases 1 to 3 precede the new action semantics of Phases 4 and 5, and the fixes found by the final review (Phase 5B) have their own commits.

| Commit | Phase | Change | Rows |
| :--- | :--- | :--- | :--- |
| `c1ad8bf` | plan | Drop the `MIXED` rule and simplify the plan: requirements 2 to 4 only, rule selection unchanged. | ENG-2, RULE-1 |
| `25811a5` | 1A | Module-owned tests behind `VIGIL_BUILD_TESTS`, one CTest entry per executable, over fake callbacks. | ENG-5, TEST-1, TEST-8 |
| `8245fed` | 1A, 1B | `RuleEndpoint` startup, gates, direct EVENT bypass, and the configuration endpoints pinned through the loop without its thread. | TEST-1, RULE-6, RULE-7 |
| `fc5189c` | 1C | Version-2 record configuration (`iSchemaVersion`, `dActions`), canonical round trips, atomic persistence with fsync before the rename, fail-closed startup. | CFG-1, CFG-3, CFG-4, CFG-6, TEST-5 |
| `45d7bfa` | 3A (pulled forward) | Every owner carries the event time and the dispatch time; rule info reports both. | EVT-3 |
| `6b9cd2e` | docs | The writer constructor-failure hot loop recorded before Phase 2A. | SAFE-5 |
| `0d404c0` | 2A | Complete only the claimed rule event; a writer constructor failure is a failed task with the cooldown and the failure count. | SAFE-1, SAFE-5, ACT-3 |
| `842610d` | 2A | The media activity count balanced on every path, the sink finished exactly once, the writers tested over a fake adapter. | SAFE-2, SAFE-5, TEST-3 |
| `6d9d0e8` | 2B | `Context::MediaBarrier`: pause and operation admission under one mutex, every writer, image capture, and thumbnail covered. | SAFE-2, SAFE-3 |
| `f8c1756` | 2B | The runtime built and torn down outside the instance lock, in dependency order; callback reentry during init and deinit pinned. | SAFE-3, SAFE-8 |
| `1a03540` | 2C | Idempotent, non-destructive directory creation; publication under the Slot's lock against removal; a taken name gets a numbered suffix. | SAFE-6, TEST-4 |
| `5d7c054` | 2C | `WaterMarks::from` uses the fallback entry for an unknown format instead of reading past its table. | SAFE-7 |
| `b0e73cb` | 3A | The clocks tick before the enable check; GPIO and TTY owners attributed; the TTY buffer leak and the overlapping-prefix miss fixed (Knuth-Morris-Pratt); the GPIO rule checks the sync callback of its signal mode. | EVT-3, RULE-4, RULE-5 |
| `7ce6718` | 4A | The action settings published as one immutable snapshot with a revision per file channel; a writer watches only its own channel's. | CFG-5, ACT-2, ACT-4 |
| `6b5ce30` | 4B | The Sound executor (Idle, Pending, Playing, Stopping), `sound_play_cb` appended to `vg_config_media_t`, `dispatch_timestamp` to `vg_rule_evt_t`. | ACT-6, ACT-7, ACT-8, API-1, API-2, EVT-1 |
| `020fd41` | 4B review | A media refresh is detected before a commit; HTTP activation dispatches with no rule lock held; the Sound worker rechecks the gates after its claim. | CFG-5, RULE-6, ACT-8 |
| `d54009b` | docs | Phase 4C split into five steps with the definitions it rests on. | ENG-2 |
| `6603a10` | 4C-1 | One `FileChannel` per capture type; `dispatchRuleEvent` dispatches into every enabled channel; `writer_created` is file acceptance only. | ACT-1, ACT-3, EVT-5, EVT-6 |
| `0294fb2` | 4C-2 | `FileChannelWorker` extracted from the Slot; publication outside the media operation and before the hold. | SAFE-4, SAFE-5 |
| `f2d54d8` | 4C-3 | In-flight budgets per filesystem; a running task continues against its own watermark. | SAFE-7, ACT-4 |
| `712a61c` | 4C-4 | One worker per file channel on each Slot; a partial start is unwound. | ACT-5, SAFE-8 |
| `6267713` | 4C-5 | File actions admitted at dispatch on the Slot registered with the context; the schedule gate published on every tick. | GATE-1, GATE-2, ACT-3 |
| `94957cf` | 4D-1 | Any combination of Photo, Video, and Sound accepted under the ALWAYS_ON invariant, with the Sound trigger gated. | CFG-4, CFG-7, RULE-6, GATE-1 |
| `cc5ac87` | 4D-2 | Rule info reports the last dispatched owner and the state of each action. | EVT-6, API-4 |
| `27e49c2` | 5A-1 | The video writer's waits for key frames and input are bounded; a clip without a key frame records nothing (Canceled). | GATE-7, SAFE-5 |
| `bc6e9d5` | 5A-2 | The task kind (triggered or continuous), owner-less continuous admission with UID 0, the continuous settings in the snapshot, the backlog hook. | GATE-3, GATE-4, GATE-6, EVT-4 |
| `a31bc37` | 5A-3 | `bContinuousRecordEnabled` accepted; clips admitted from the rule loop on every tick; triggered Video suppressed meanwhile. | GATE-3, GATE-5, CFG-2, TEST-4 |
| `29a8a1c` | 5B-1 | The single-action formats and the ALWAYS_ON pacing regressed; the triggered pre-roll documented. | ACT-2, TEST-1 |
| `28455d8` | 5B-2 | Negative and oversized `iDebounceTimes` rejected; the SED rule pinned as observed over a fake socket peer. | CFG-8, RULE-3, TEST-2 |
| `c44290d` | 5B-3 | The C API boundary audited: pointer and status defects fixed, the callback contract stated in the headers (versions `2026.10.20`). | API-3, EVT-7 |
| `134df6b` | 5B-3b | A callback on the thread of a C API call may call the C API; init and deinit return EDEADLK there. | API-3, SAFE-3 |
| `bd74891` | 5B-4 | The README option list, the architecture graph checked against the code, the capability limits. | API-4, API-5 |
| `6195994` | 5B-5a | Manual capture pinned through the real endpoint; a capture into a new directory, a failed capture's file, and a double completion fixed. | TEST-4, SAFE-5, SAFE-7 |
| this commit | 5B-5b | Fresh standalone builds, the Timer pacing case, the evidence pass over the open rows, this document. | ENG-1 to ENG-4, ENG-6, RULE-1, RULE-2, RULE-6, RULE-7, EVT-4, ACT-3, ACT-4, GATE-1, SAFE-5, TEST-1, TEST-3, TEST-5 to TEST-9 |

## 2. Interfaces, documents, versions, fixtures, examples

**C API** (`include/vigil_ctypes.h`, `include/vigil_capi.h`, both `2026.10.20`). `vg_sta_e (*sound_play_cb)(void *user_data)` is appended to `vg_config_media_t` and `int64_t dispatch_timestamp` to `vg_rule_evt_t`; no enum value and no other layout changed. Growing `vg_config_media_t` shifts the `storage` field of `vg_config_t`, and the initialization API has no size or version negotiation: the library and every caller are rebuilt together, and no old binary can be mixed with the new layout. The headers state the whole callback contract: any nonzero status is a failure, which callbacks are required, what is borrowed and until when, which thread calls what under which lock, that `key_frame` is the value 1, that `log_level` is reserved, and that any function but init and deinit may be called from a callback unless the callback's own description says otherwise (the log, unmount, GPIO, and estimate callbacks may not, and media callbacks may not call the scoped suspension).

**HTTP API** (`docs/minimal-http-api.md`, version 2.0.0, revised 2026-10-20). The general rule configuration is the version-2 document with `bContinuousRecordEnabled` and `dActions` (Photo, Video, Sound); rule info adds `dLastDispatchedOwner`, `dActionStates`, `iDispatchTimestamp`, and the owner fields the code emitted before; the record rule prose matches the implementation (the debounce counter, the direct EVENT fields, the SED filter, the Always-On invariant); the Capture API notes state the status semantics, the cache name, the shared budget, and what a stop does; the general notes carry the capability limits.

**Configuration files.** `record_config.json` is version 2 only: another version or the old `dWriterConfig` shape is rejected and the file is never rewritten, so an external manager migrates older files; a stored pair the selected ALWAYS_ON rule cannot pace keeps rules disabled and continuous recording off at startup, with both files in place. `schedule_rule_config.json` and `record_rule_config.json` are unchanged. The fixtures are the documents the tests post and load: `tests/test_record_config.cpp` (every combination, the rejections), `tests/test_rule_endpoint.cpp` (startup files, the peripheral and HTTP rule selections, the invariant pairs), `tests/test_inference_rules.cpp` and `tests/test_peripheral_rules.cpp` (the inference and SED rule configurations), `tests/test_capi.cpp` (through the C API).

**Documents.** `README.txt` (requirements, every CMake option, the test and sanitizer builds), `docs/architecture.dot` (checked against the code in Phase 5B-4), `docs/PLAN.md` (the plan with the status of every section and unit), `docs/TODO.md` (the checklist with evidence), this document.

**Example.** `example/main.cpp` zero-initializes `vg_config_t`, sets `sound_play_cb` and `on_file_event_cb`, and prints the event time and the dispatch time of a rule event; it is a demonstration with dummy callbacks, not a test.

## 3. Build and test commands and results

Vigil builds and runs on Linux only (fanotify, netlink uevents, libblkid, Unix domain sockets). Everything below ran in the `dev` container described in `docs/PLAN.md` section 12.1 (Debian 13, aarch64, GCC 14.2.0, CMake 3.31.6, Ninja, Protobuf 3.21.12, libblkid 2.41.5, autoconf for the bundled jemalloc), on the mirror of this checkout, with out-of-tree build directories.

### 3.1 Fresh standalone builds (TEST-8)

Four configurations from empty build directories on 2026-10-20, nothing reused, examples and tests on, the demo run from an empty working directory:

```sh
SRC=~/dev/Vigil; OUT=/root/build/5b5b-fresh
common="-G Ninja -DVIGIL_USE_BUILDROOT_SYSROOT=OFF -DVIGIL_BUILD_EXAMPLES=ON -DVIGIL_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug"
cmake -S $SRC -B $OUT/debug    $common -DVIGIL_ENABLE_JEMALLOC=OFF
cmake -S $SRC -B $OUT/asan     $common -DVIGIL_ENABLE_JEMALLOC=OFF \
    "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer" \
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake -S $SRC -B $OUT/tsan     $common -DVIGIL_ENABLE_JEMALLOC=OFF \
    "-DCMAKE_C_FLAGS=-fsanitize=thread" "-DCMAKE_CXX_FLAGS=-fsanitize=thread"
cmake -S $SRC -B $OUT/jemalloc $common -DVIGIL_ENABLE_JEMALLOC=ON
for c in debug asan tsan jemalloc; do
    cmake --build $OUT/$c -j7
    ctest --test-dir $OUT/$c -N                   # Total Tests: 12
    ctest --test-dir $OUT/$c --output-on-failure  # 100% tests passed, 0 tests failed out of 12
    (mkdir -p $OUT/run-$c && cd $OUT/run-$c && $OUT/$c/vigil_example)   # exit 0
done
```

| Configuration | Configure and build (7 jobs) | Warnings | Discovered | Cases | Sanitizer reports | Demo |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| Debug, allocator off | exit 0, 11 s | 0 | 12 | 143 passed, 0 failed | none expected | exit 0 |
| ASan/UBSan, allocator off | exit 0, 16 s | 0 | 12 | 143 passed, 0 failed | 0 | exit 0 |
| TSan, allocator off | exit 0, 10 s | 0 | 12 | 143 passed, 0 failed | 0 | exit 0 |
| jemalloc on | exit 0, 53 s | 0 | 12 | 143 passed, 0 failed | none expected | exit 0 |

The twelve executables and their cases: inference rules 12, schedule rule 7, context 21, record configuration 16, rule endpoint 31, writers 6, C API 9, index manager helpers 3, peripheral rules 11, sound executor 10, channel worker 12, capture endpoint 5. Every earlier unit ran the same four configurations incrementally (the `/root/build/<unit>-matrix.sh` scripts; the counts per unit are in `docs/PLAN.md` section 12.1 and the commit messages).

### 3.2 What the tests exercise and what they inject

The tests run the production rule, configuration, dispatch, executor, worker, writer, endpoint, and C API code over `tests/support/`: a `vg_config_t` whose only live callbacks are logging and rule event recording (`fake_config.hpp`), fake media callbacks with a knob per outcome and hooks that run inside a callback (`fake_media.hpp`), a fake Slot admission with a fake filesystem and budget (`fake_slot.hpp`), temporary directories, and a Unix socket peer for the SED rule. No test needs a peripheral, a storage device, a media sink, elevated privileges, or a sleep as evidence. Failures injected:

- Writers (`test_writers.cpp`, `test_channel_worker.cpp`, `test_capture_endpoint.cpp`): create, write, finish, and image capture callbacks returning a nonzero status (EIO), missing callbacks, a pause from inside the create callback, key frame and input-loss waits with injected bounds, a stop and a settings change between frames, a stale task, a refused admission, the failure cooldown and the consecutive failure limit with injected constants, a reservation that leaves no headroom, a missing output directory, a failing callback that leaves a partial file, a writer that cannot be built.
- Configuration and endpoints (`test_record_config.cpp`, `test_rule_endpoint.cpp`): a missing or other schema version, the old shape, missing fields, wrong types, zero, fractional, and overflowing intervals, an unwritable configuration directory (persistence failure before the rename), corrupt and missing files at startup, a pair that violates the ALWAYS_ON invariant, both POST orders and concurrent POSTs, a revision conflict from a media refresh, a failed record rule switch (a save that cannot complete) with a GPIO pin held.
- Rules (`test_inference_rules.cpp`, `test_schedule_rule.cpp`): malformed filters, negative and oversized debounce values, missing, unrelated, and direct EVENT input, queue resets, gate closures, malformed schedule ranges.
- Peripherals (`test_peripheral_rules.cpp`): a failing or absent GPIO pin query, a pin list that must be freed on every parse outcome, an acquire that fails, a TTY entry without a socket path, fragmented and overlapping TTY commands and reads larger than the buffer, SED envelopes that are malformed, oversized, zero-length, ignored, inactive, or cut off, a peer that disconnects, with the reconnect backoff observed.
- Sound (`test_sound_executor.cpp`): a null callback, a callback that fails, retriggers while Pending and Playing, disable and stop before and during a callback.
- Lifetime (`test_capi.cpp`): a null configuration and configuration directory, frames without a payload, unreleased responses, reentry from a callback during init and deinit, a nested C API call from the RULE callback of an activation, init and deinit from that callback (EDEADLK), request bodies over the 1 MiB bound, the real runtime with a nonexistent internal device and every block device hidden.

### 3.3 Sanitizer limitations

ASan/UBSan and TSan run in separate configurations with the allocator off: `VIGIL_ENABLE_JEMALLOC=ON` replaces the global `new` and `delete`, which the sanitizers' allocators do not cover, so the jemalloc configuration runs without a sanitizer and the sanitizer configurations without jemalloc. TSan reports a race only on the interleavings the tests reach; the threaded cases drive the workers deterministically through hooks and bounded waits, so a race that needs a timing the tests never produce would not be seen. UBSan runs with the default `-fsanitize=undefined` checks. The examples run under the sanitizers only as the five-second demo, which exercises no rule, input, or writer.

### 3.4 Validation that was not run

- No real device: no block device was mounted, formatted, checked (`fsck`), indexed with fanotify, rotated, freed, or hot-plugged over netlink; no relay export was fetched; the storage code ran only against a nonexistent internal device with every scanned device hidden (`test_capi.cpp`), and the Slot's own paths (publication under its lock, quota and rotation, the failure state, fault cleanup of its cache directory) are covered by reading.
- No real peripheral: GPIO pins, TTY sockets with a real peer, and the AcousticsLab daemon were replaced by fakes; the SED backoff (its 100 ms start, doubling, and reset by a valid frame) is observed over a fake peer, while the 1-second connect timeout, the 5-second frame read timeout, and the 5-second backoff cap are stated from reading.
- No real media pipeline: callback durations, real-device latency, the gap between clips, the pre-roll on a real stream, and memory and CPU use were not measured; the 10 ms rule evaluation and 50 ms worker probe periods are targets, not measurements.
- Not built: the Buildroot cross-build (`VIGIL_USE_BUILDROOT_SYSROOT`), `VIGIL_USE_SYSTEM_JEMALLOC`, the jemalloc stats and profiling options, `VIGIL_USE_SYSTEM_NLOHMANN_JSON`, and the install step. No Release build was tested; every run is a Debug build.
- No long run: the longest test waits about three seconds (the Timer pacing case, in real time, as there is no fake steady clock); there was no soak test, and the ten-second production waits of the video writer were never waited out (the writer tests run one production-bound wait, broken by the gate; the bounds themselves are pinned with injected shorter ones).

## 4. Deviations, open edge cases, and limits needing review

### 4.1 Deliberate behavior changes

Each is pinned with a test and dated in `docs/PLAN.md`; the default of the plan was to keep the implemented behavior and correct the documentation, and these are the exceptions.

1. **Record configuration version 2 only** (1C): the old `dWriterConfig` shape and any other version are rejected and the file is never rewritten; an unusable file or a pair the selected ALWAYS_ON rule cannot pace keeps rules disabled and continuous recording off at startup.
2. **Independent actions** (4A to 4D): Photo and Video run on their own intervals and may both publish for one event; Sound plays through the host's callback with no queue and no retry; `writer_created` means at least one file channel accepted the event; the RULE callback fires on every dispatched owner, also when every file action was refused.
3. **ALWAYS_ON invariant** (4D-1): with ALWAYS_ON selected the actions are limited to what it paces (no Sound, no continuous recording, at most one file action, exactly one while rules are enabled), checked by both configuration POSTs under one lock.
4. **Continuous recording** (5A): `bContinuousRecordEnabled` records clips back to back while the schedule is open, whatever `bRuleEnabled` is, with no RULE callback and `event_uid` 0; triggered Video is suppressed meanwhile.
5. **A clip without a key frame records nothing** (5A-1): Canceled with no file, instead of an empty file published as completed; the waits for the first and the closing key frame and for input are bounded at 10 seconds.
6. **A writer constructor failure is a failed task** (2A) with the cooldown and the failure count, instead of a hot retry loop; a task completes only the event it claimed.
7. **Publication before the cooldown, no overwrite** (2A, 2C): a recorded file is published before the hold; a taken name gets a numbered suffix; directory creation never removes what is in the way.
8. **Media suspension covers everything** (2B, 4B): image capture, thumbnail generation, and Sound playback join the video writers under the barrier; `vg_media_scoped_suspend_write(true)` waits for all of them and must not be called from a media callback.
9. **Deinit outside the instance lock** (2B) and **nested C API calls** (5B-3b): a callback may call any C API function but init and deinit, also on the thread of the call that invoked it; init and deinit return EDEADLK there. The GPIO callbacks and `log_cb` keep their constraint (no C API calls).
10. **Frames without a payload are refused** (5B-3): `vg_media_enqueue_*` returns EINVAL for a null buffer or a zero length instead of keeping the host's pointer.
11. **`VG_HTTP_API_SAFE_RELEASE_RESPONSE` clears `free_cb`** (5B-3), so a released response can be reused; a response that is neither zero-initialized nor released is refused with EINVAL.
12. **Peripheral rule fixes** (3A): the TTY matcher finds overlapping occurrences and frees its buffer; the GPIO rule requires the sync callback of its signal mode; a failed rule switch destroys the failed rule before rebuilding the committed one (5B-3).
13. **Parse-time bounds** (5B-2): negative and oversized `iDebounceTimes` are rejected; the empty SED class filter keeps matching nothing, now documented.
14. **Direct EVENT owners carry the input time** (3A) and every owner a dispatch time (4B); GPIO and TTY owners are attributed (the signal or the command as the rule ID, the pin or the TTY as the source).
15. **A refused HTTP activation leaves nothing pending** (4C-5): the next activation is admitted afresh; a rule-only or Sound-only activation needs no storage (4D-1).
16. **Manual capture** (5B-5a): a capture into a directory that does not exist yet works; a failed capture leaves no file (a host whose callback writes a usable file and then returns nonzero loses it); a writer that cannot be built no longer drops a capture started meanwhile.

### 4.2 Open edge cases and retained limitations

- **A Slot's publication failure after a completed write** (by reading, `Slot::publish`): the cache file is removed and no FILE callback fires, while a thumbnail already moved into `.thumb` before the file rename failed stays there, orphaned; the task keeps the Completed status its writer ended with, so a lost publication counts toward neither the cooldown nor the failure limit. It needs a mounted Slot to pin.
- **A Failed task's partial cache file and a failed thumbnail stay** in the Slot's cache directory until the Slot is next enabled, which clears it.
- **Either channel's failure limit marks the Slot failed**: ten failed tasks without a completed one between them take the Slot to `ERROR`, which stops the other channel and continuous recording until the Slot is disabled and enabled again (reselect it, eject and reinsert a removable device, or restart the service). A persistently failing Photo callback therefore ends Video recording on that Slot (the retained limitation of `docs/PLAN.md` section 7.2).
- **Rotation or removal during publication** is excluded by the Slot's publication lock, by reading; it needs a mounted Slot to exercise.
- **Partial initialization** has no injection seam: a constructor that throws destroys the members built so far, and `Slot::enable` unwinds the threads already started; covered by reading.
- **`basic.log_level` is reserved**: filtering is compile-time (`VG_LOG_LEVEL`).
- **The manual capture output check is lexical**: the path must start with `/mnt` or the configured internal mount point as strings.
- **`doMount` detaches and clears an existing mount path**, the internal mount point included.
- **A Slot in `ERROR` does not recover on its own** (the manager thread exits), see above.
- **No fake steady clock**: the ALWAYS_ON and Timer pacing cases wait for the context's time in real time (a fraction of a second and about three seconds) with bounded loops.
- **`vigil_test_capture_endpoint` needs more than 532 MiB free** on the temporary directory's filesystem, the critical watermark of one image.
- **The C ABI changed** with the two appended fields: rebuild the library and every caller together; version strings do not make old binaries compatible.

### 4.3 Callback and performance limits

- Media callbacks run on the Photo and Video workers and the capture thread and block them for their duration; `sound_play_cb` runs on the Sound worker and must return in bounded time, since stop and `vg_service_deinit` join it; `max_bytes_estimate_cb` runs on the init, POST, and resume threads, under the suspension lock on a resume, and must be a pure function.
- `log_cb` may run on any thread under internal locks and `on_unmount_cb` under the Slot locks: neither may call the C API; the GPIO callbacks run under the rule lock on a record rule POST and the rule thread, with the same rule.
- `on_rule_event_cb` runs on the dispatching thread (the rule thread or the HTTP request thread) with no lock, and `on_file_event_cb` on the publishing worker; both are best-effort, a FILE callback may precede its RULE callback, and a rule event is followed by zero, one, or two files.
- The rule loop targets a 10 ms period and the workers and the capture thread 50 ms probes; a callback that takes longer than the period delays only its own thread. None of this was measured on a device.

## 5. The checklist

`docs/TODO.md` is updated: every row is checked with its evidence (the test names and the plan sections), and the rows whose last items are limitations rather than tests (SAFE-5, TEST-3, TEST-4) say so and point here.
