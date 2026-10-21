// Characterization tests for Context: inference intake, the per-tick input hand-off, rule event
// creation/reporting, and the owner identity that the C event projection is built from
// (docs/PLAN.md sections 5.1, 6.1, 6.2; matrix R1 and W1). Everything here is today's behavior.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/rules.hpp"
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_slot.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::rules;
using namespace vigil::tests;

namespace {

// The settings of one file channel, as a configuration with that action enabled would publish them.
std::shared_ptr<FileWriterSettings> ChannelSettings(const Context &ctx, CaptureType type,
    std::chrono::milliseconds interval = kRecordWriterDefaultIntervalMs)
{
    return FileWriterSettings::create(type, type == CaptureType::Video ? kRecordWriterDefaultFormat : "JPG", interval,
        kRecordWriterPrefixDefault, kVideoClipLengthDefault, &ctx.c_api.media);
}

// Publishes a snapshot with the given file channels, built directly with the intervals each case needs, and
// with continuous recording configured when `continuous` is set.
void Publish(Context &ctx, std::shared_ptr<FileWriterSettings> photo, std::shared_ptr<FileWriterSettings> video,
    bool continuous = false)
{
    auto &shared = ctx.action_settings;
    VG_REQUIRE(shared.update(shared.getRevision(),
        ActionSettings::create(continuous, false, std::move(photo), std::move(video), &ctx.c_api.media)));
}

struct Fixture
{
    Context ctx;
    FakeSlotAdmission slot; // registered and ready: admission needs a usable Slot (docs/PLAN.md section 8.1)

    // With rules enabled and the Video channel enabled: a dispatch accepts into enabled channels only, and the
    // system enables one whenever rules are enabled (docs/PLAN.md section 4.1).
    Fixture() : ctx(MakeFakeConfig()), slot(ctx)
    {
        RuleEventRecorder::clear();
        ctx.record_rule_enabled.store(true);
        Publish(ctx, nullptr, ChannelSettings(ctx, CaptureType::Video));
    }
};

// A direct discrete EVENT: no payload, only a source.
InferenceResult Event(int64_t ts, const std::string &source)
{
    InferenceResult r;
    r.timestamp_ms = ts;
    r.source_id = source;
    return r;
}

// A FRAME result: a (possibly empty) detection payload.
InferenceResult Frame(int64_t ts, const std::string &source)
{
    InferenceResult r;
    r.timestamp_ms = ts;
    r.source_id = source;
    r.data = DetectionResultVec {};
    return r;
}

std::shared_ptr<EventOwner> Owner(vg_rule_type_e type, const std::string &id = "", const std::string &source = "",
    EventOwner::SourceDelivery delivery = EventOwner::SourceDelivery::Event)
{
    return std::make_shared<EventOwner>(type, id, source, delivery);
}

// What the worker does at the end of its task: completes the task its channel holds.
void CompleteCurrent(Context &ctx, WriteEvent::Status status)
{
    if (const auto [task, held] = ctx.file_channels.latest(); held)
        ctx.file_channels.of(task->type).complete(task, status);
}

} // namespace

VG_TEST_CASE(frames_are_dropped_without_subscribers_but_events_are_kept)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);

    f.ctx.enqueueInferenceResult(Frame(1, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });
    f.ctx.enqueueInferenceResult(Event(2, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 1 });

    f.ctx.inference_subscribers.store(1); // what an InferenceRule does for its lifetime
    f.ctx.enqueueInferenceResult(Frame(3, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 2 });
}

VG_TEST_CASE(disabled_rules_clear_and_reject_inference_input)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.inference_subscribers.store(1);
    f.ctx.enqueueInferenceResult(Frame(1, "cam0"));
    f.ctx.enqueueInferenceResult(Event(2, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 2 });

    f.ctx.record_rule_enabled.store(false);
    f.ctx.enqueueInferenceResult(Frame(3, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 }); // cleared, and the new entry dropped
    f.ctx.enqueueInferenceResult(Event(4, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });
}

VG_TEST_CASE(full_queue_drops_the_oldest_entries)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.inference_subscribers.store(1);
    for (int64_t ts = 0; ts <= static_cast<int64_t>(kInferenceQueueSize); ++ts)
        f.ctx.enqueueInferenceResult(Frame(ts, "cam0"));
    VG_CHECK_EQ(f.ctx.inference_queue.size(), kInferenceQueueSize);
    VG_CHECK_EQ(f.ctx.inference_queue.front().timestamp_ms, int64_t { 1 });
    VG_CHECK_EQ(f.ctx.inference_queue.back().timestamp_ms, static_cast<int64_t>(kInferenceQueueSize));
}

VG_TEST_CASE(tick_hands_over_one_entry_per_call_in_fifo_order)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.enqueueInferenceResult(Event(1, "cam0"));
    f.ctx.enqueueInferenceResult(Event(2, "cam1"));
    f.ctx.enqueueInferenceResult(Event(3, "cam0"));

    const auto before = f.ctx.shared_time.steadyTime();
    const auto wall_before = EventOwner::NowMs();
    f.ctx.tick();
    VG_CHECK(f.ctx.shared_time.steadyTime() > before);     // the clocks advance on an enabled tick
    VG_CHECK(f.ctx.shared_time.unixTime() >= wall_before); // the input's time (1) is not the current time
    VG_REQUIRE(f.ctx.shared_inference_result.has_value());
    VG_CHECK_EQ(f.ctx.shared_inference_result->timestamp_ms, int64_t { 1 });
    VG_CHECK_EQ(f.ctx.shared_inference_result->source_id, "cam0");
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 2 });

    f.ctx.tick();
    VG_REQUIRE(f.ctx.shared_inference_result.has_value());
    VG_CHECK_EQ(f.ctx.shared_inference_result->timestamp_ms, int64_t { 2 });
    f.ctx.tick();
    VG_REQUIRE(f.ctx.shared_inference_result.has_value());
    VG_CHECK_EQ(f.ctx.shared_inference_result->timestamp_ms, int64_t { 3 });
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });

    f.ctx.tick(); // nothing queued: no stale result is kept
    VG_CHECK(!f.ctx.shared_inference_result.has_value());
}

VG_TEST_CASE(tick_while_disabled_resets_input_and_cancels_the_current_event)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.enqueueInferenceResult(Event(1, "cam0"));
    f.ctx.enqueueInferenceResult(Event(2, "cam0"));
    f.ctx.tick();
    VG_CHECK(f.ctx.shared_inference_result.has_value());
    const auto owner = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner));
    VG_CHECK(f.ctx.file_channels.video.current() != nullptr);

    f.ctx.record_rule_enabled.store(false);
    const auto before = f.ctx.shared_time.steadyTime();
    f.ctx.tick();
    VG_CHECK(f.ctx.shared_time.steadyTime() > before); // the clocks advance on a disabled tick too (since 2026-10-19)
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });
    VG_CHECK(!f.ctx.shared_inference_result.has_value());
    VG_CHECK(f.ctx.file_channels.video.current() == nullptr);
    const auto last = f.ctx.file_channels.video.last();
    VG_REQUIRE(last != nullptr);
    VG_CHECK(last->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK_EQ(last->event->owner_uid, owner->uid);
    VG_CHECK(last->owner == owner);

    // The tick's cancel after a commit enabled rules again, as a tick that read the flag disabled runs it: the
    // cancel rereads the flag under the channel lock, so a task accepted under the enabled rules since is kept
    // (2026-10-21).
    f.ctx.record_rule_enabled.store(true);
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER)));
    f.ctx.file_channels.cancelPendingTriggered();
    VG_CHECK(f.ctx.file_channels.video.current() != nullptr);
    f.ctx.file_channels.cancelPending();
}

VG_TEST_CASE(reset_inference_clears_everything_or_only_one_source)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.enqueueInferenceResult(Event(1, "cam0"));
    f.ctx.enqueueInferenceResult(Event(2, "cam1"));
    f.ctx.enqueueInferenceResult(Event(3, "cam0"));
    f.ctx.enqueueInferenceResult(Event(4, "cam1"));

    f.ctx.resetInference("cam1");
    VG_REQUIRE(f.ctx.inference_queue.size() == size_t { 2 });
    VG_CHECK_EQ(f.ctx.inference_queue.front().timestamp_ms, int64_t { 1 });
    VG_CHECK_EQ(f.ctx.inference_queue.back().timestamp_ms, int64_t { 3 });

    f.ctx.resetInference("nobody"); // unknown source: no change
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 2 });

    f.ctx.resetInference();
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });
}

VG_TEST_CASE(rule_event_is_reported_on_every_dispatch_but_the_writer_event_is_created_once)
{
    Fixture f;
    auto &video = f.ctx.file_channels.video;
    const auto owner1
        = std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, int64_t { 5000 });
    VG_CHECK(f.ctx.dispatchRuleEvent(owner1));
    const auto current = video.current();
    VG_REQUIRE(current != nullptr);
    VG_CHECK(current->type == CaptureType::Video);
    VG_CHECK(current->owner == owner1);
    VG_CHECK(current->settings == f.ctx.action_settings.get().settings->video); // bound at acceptance
    VG_CHECK_EQ(current->settings_revision, f.ctx.action_settings.get().video_revision);
    VG_CHECK_EQ(current->event->owner_uid, owner1->uid);
    VG_CHECK(current->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK_EQ(current->event->timestamp, int64_t { 5000 }); // the owner's event time (the clock before 2026-10-19)
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner1);

    // A second owner while one task is held: reported and recorded as the last dispatched owner, but no new task.
    const auto owner2
        = std::make_shared<EventOwner>(VG_RULE_TYPE_HTTP, "", "", EventOwner::SourceDelivery::Event, int64_t { 6000 });
    VG_CHECK(!f.ctx.dispatchRuleEvent(owner2));
    VG_CHECK(video.current() == current);
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner2);

    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 2 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(events[0].uid, owner1->uid);
    VG_CHECK_EQ(events[0].timestamp, int64_t { 5000 });
    VG_CHECK_EQ(events[0].dispatch_timestamp, owner1->dispatch_timestamp); // appended 2026-10-19 (Phase 4B)
    VG_CHECK(!events[0].id_is_null);                                       // borrowed strings are empty, never null
    VG_CHECK_EQ(events[0].id, "");
    VG_CHECK(!events[0].source_id_is_null);
    VG_CHECK_EQ(events[0].source_id, "");
    VG_CHECK(!events[0].source_delivery_is_null);
    VG_CHECK_EQ(events[0].source_delivery, "EVENT");
    VG_CHECK_EQ(events[1].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(!events[1].writer_created);
    VG_CHECK_EQ(events[1].uid, owner2->uid);

    // Completing the task frees the channel and keeps the task as "last".
    CompleteCurrent(f.ctx, WriteEvent::Status::Completed);
    VG_CHECK(video.current() == nullptr);
    const auto last = video.last();
    VG_REQUIRE(last != nullptr);
    VG_CHECK(last == current);
    VG_CHECK(last->event->status.load() == WriteEvent::Status::Completed);
    {
        const auto [task, held] = f.ctx.file_channels.latest();
        VG_CHECK(task == last);
        VG_CHECK(!held);
    }

    const auto owner3 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner3));
    VG_CHECK(video.current()->owner == owner3);
    VG_CHECK(video.current()->sequence > last->sequence);

    // A null owner is ignored without a callback.
    VG_CHECK(!f.ctx.dispatchRuleEvent(nullptr));
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner3);
}

VG_TEST_CASE(claim_cancel_and_complete_are_identity_aware)
{
    Fixture f;
    auto &video = f.ctx.file_channels.video;
    const auto registration = f.slot.registration();
    VG_CHECK(video.claim(registration) == nullptr); // nothing pending
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER)));
    VG_CHECK(video.claim(registration + 1) == nullptr); // another Slot's worker: not its task to run
    VG_CHECK(video.current()->event->status.load() == WriteEvent::Status::Pending);
    const auto a = video.claim(registration);
    VG_REQUIRE(a != nullptr);
    VG_CHECK(a->event->status.load() == WriteEvent::Status::Writing);
    VG_CHECK(video.claim(registration) == nullptr); // claimed once
    video.cancelPending();                          // a claimed task belongs to its worker
    VG_CHECK(video.current() == a);
    f.ctx.record_rule_enabled.store(false);
    f.ctx.tick(); // the disabled tick cancels only a pending task
    VG_CHECK(video.current() == a);
    video.complete(a, WriteEvent::Status::Interrupted);
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(video.last() == a);
    VG_CHECK(a->event->status.load() == WriteEvent::Status::Interrupted);
    f.ctx.record_rule_enabled.store(true); // the gate applies at admission (docs/PLAN.md section 8.1)

    // A newer task B is never touched by the stale completion of A.
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_HTTP)));
    const auto b = video.current();
    VG_REQUIRE(b != nullptr);
    video.complete(a, WriteEvent::Status::Failed);
    VG_CHECK(video.current() == b);
    VG_CHECK(a->event->status.load() == WriteEvent::Status::Interrupted);
    video.complete(nullptr, WriteEvent::Status::Failed);
    VG_CHECK(video.current() == b);

    // A pending task is what cancelPending (the teardown; the disabled tick's cancelPendingTriggered likewise)
    // retires.
    f.ctx.file_channels.cancelPending();
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(video.last() == b);
    VG_CHECK(b->event->status.load() == WriteEvent::Status::Canceled);
}

VG_TEST_CASE(channels_hold_tasks_independently_and_a_stale_task_never_touches_a_newer_one)
{
    // Two enabled channels, built directly (docs/PLAN.md section 7.2).
    Fixture f;
    auto &photo = f.ctx.file_channels.photo;
    auto &video = f.ctx.file_channels.video;
    Publish(f.ctx, ChannelSettings(f.ctx, CaptureType::Image), ChannelSettings(f.ctx, CaptureType::Video));

    const auto owner1 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner1));
    const auto a = photo.current(), b = video.current();
    VG_REQUIRE(a != nullptr && b != nullptr);
    VG_CHECK(a->type == CaptureType::Image && b->type == CaptureType::Video);
    VG_CHECK(a->owner == owner1 && b->owner == owner1);
    VG_CHECK(a->event != b->event); // one task per channel, each with its own event
    VG_CHECK_EQ(a->event->owner_uid, owner1->uid);
    VG_CHECK_EQ(b->event->owner_uid, owner1->uid);
    VG_CHECK(b->sequence > a->sequence); // accepted in channel order: Photo, then Video
    {
        const auto [task, held] = f.ctx.file_channels.latest();
        VG_CHECK(task == b && held); // the held task accepted last
    }

    // Completing the Video task leaves the Photo task held; completing Photo frees everything.
    VG_CHECK(photo.claim(f.slot.registration()) == a);
    video.complete(b, WriteEvent::Status::Completed);
    VG_CHECK(video.current() == nullptr && photo.current() == a);
    VG_CHECK(f.ctx.file_channels.holdsTask());
    {
        const auto [task, held] = f.ctx.file_channels.latest();
        VG_CHECK(task == a && held);
    }
    photo.complete(a, WriteEvent::Status::Completed);
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    {
        const auto [task, held] = f.ctx.file_channels.latest();
        VG_CHECK(task == b && !held); // of the retired tasks, the one accepted last
    }

    // Newer tasks A2 and B2: a stale completion of A, a completion addressed to the wrong channel, and a
    // cancellation in one channel never touch the other channel's task.
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER)));
    const auto a2 = photo.current(), b2 = video.current();
    VG_REQUIRE(a2 != nullptr && b2 != nullptr);
    photo.complete(a, WriteEvent::Status::Failed);
    video.complete(a2, WriteEvent::Status::Failed);
    VG_CHECK(photo.current() == a2 && video.current() == b2);
    VG_CHECK(a->event->status.load() == WriteEvent::Status::Completed);
    video.cancelPending();
    VG_CHECK(photo.current() == a2 && video.current() == nullptr);
    VG_CHECK(a2->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK(b2->event->status.load() == WriteEvent::Status::Canceled);
    f.ctx.file_channels.cancelPending();
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK(a2->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(photo.last() == a2 && video.last() == b2);
}

VG_TEST_CASE(dispatch_accepts_into_every_enabled_channel_and_reports_either_acceptance)
{
    Fixture f;
    auto &photo = f.ctx.file_channels.photo;
    auto &video = f.ctx.file_channels.video;
    Publish(f.ctx, ChannelSettings(f.ctx, CaptureType::Image), ChannelSettings(f.ctx, CaptureType::Video));

    // writer_created is file acceptance in at least one channel (docs/PLAN.md section 6.2).
    const auto owner1 = Owner(VG_RULE_TYPE_TIMER);
    const auto owner2 = Owner(VG_RULE_TYPE_HTTP);
    const auto owner3 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner1));             // both channels accept
    VG_CHECK(!f.ctx.dispatchRuleEvent(owner2));            // both hold a task
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner2); // recorded whatever the channels did
    video.complete(video.current(), WriteEvent::Status::Completed);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner3)); // Video accepts; Photo still holds owner1's task
    VG_CHECK(photo.current()->owner == owner1);
    VG_CHECK(video.current()->owner == owner3);
    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 3 });
    VG_CHECK(events[0].writer_created && !events[1].writer_created && events[2].writer_created);
    VG_CHECK_EQ(events[1].uid, owner2->uid);

    // A task is bound to the settings the dispatcher loaded: a channel whose settings were republished since
    // refuses it, and the other channel is unaffected.
    f.ctx.file_channels.cancelPending();
    const auto loaded = f.ctx.action_settings.get();
    Publish(f.ctx, loaded.settings->photo, ChannelSettings(f.ctx, CaptureType::Video, std::chrono::milliseconds(1234)));
    VG_CHECK(video.tryAccept(owner1, loaded) == nullptr);
    VG_CHECK(photo.tryAccept(owner1, loaded) != nullptr);
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner1)); // the current snapshot: Video accepts with the new settings
    VG_CHECK_EQ(video.current()->settings->interval.count(), 1234L);
    VG_CHECK(photo.current()->settings == loaded.settings->photo);
}

VG_TEST_CASE(admission_needs_a_usable_slot_with_headroom_and_a_rejected_owner_leaves_nothing_pending)
{
    // The deliberate change of docs/PLAN.md section 8.1: a file action is admitted at dispatch, on the registered
    // Slot with its budget reserved, or rejected with nothing pending, so the next owner proceeds instead of
    // waiting behind a pending event. The task binds the Slot it was admitted on: closing that Slot's admission
    // retires it, and a reopened Slot has a new registration.
    Fixture f;
    auto &video = f.ctx.file_channels.video;
    const auto task_bytes = f.ctx.action_settings.get().settings->video->watermarks.task_bytes;
    const auto in_flight = [&] { return f.ctx.in_flight_budgets.inFlight(FakeSlotAdmission::kFilesystem); };

    f.slot.ready = false; // no usable Slot: reported, nothing accepted
    const auto owner1 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(!f.ctx.dispatchRuleEvent(owner1));
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner1);
    VG_CHECK_EQ(in_flight(), FileSize { 0 });
    f.slot.ready = true;
    f.slot.free_bytes = f.ctx.action_settings.get().settings->watermarks.critical_remain_bytes; // no headroom
    VG_CHECK(!f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER)));
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    VG_CHECK_EQ(in_flight(), FileSize { 0 });
    {
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 2 });
        VG_CHECK(!events[0].writer_created && !events[1].writer_created);
        VG_CHECK_EQ(events[0].uid, owner1->uid);
    }

    f.slot.free_bytes = std::numeric_limits<FileSize>::max() / 2; // the next owner proceeds
    const auto owner2 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner2));
    const auto task = video.current();
    VG_REQUIRE(task != nullptr);
    VG_CHECK(task->owner == owner2);
    VG_CHECK_EQ(task->slot_registration, f.slot.registration());
    VG_CHECK_EQ(task->reservation.bytes(), task_bytes); // reserved at acceptance, carried by the task
    VG_CHECK_EQ(in_flight(), task_bytes);

    const auto old_registration = f.slot.registration();
    f.slot.close(); // the Slot is disabled: what it accepted and no worker claimed is retired, with its budget
    VG_CHECK(video.current() == nullptr && video.last() == task);
    VG_CHECK(task->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK_EQ(in_flight(), FileSize { 0 });
    VG_CHECK(!f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER))); // no Slot registered
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    f.slot.open();
    VG_CHECK(f.slot.registration() > old_registration);
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER)));
    VG_CHECK_EQ(video.current()->slot_registration, f.slot.registration());
    VG_CHECK(video.claim(old_registration) == nullptr); // the old Slot's worker cannot run it
    VG_CHECK(video.claim(f.slot.registration()) != nullptr);
}

VG_TEST_CASE(admission_rejects_when_a_gate_closed_between_evaluation_and_dispatch)
{
    // The gates are read inside the admission section (docs/PLAN.md section 8.1): rules disabled, the schedule
    // closed, or media paused since the caller evaluated them reject the file action, with the owner still
    // reported and nothing pending.
    Fixture f;
    const auto rejected = [&] {
        const auto owner = Owner(VG_RULE_TYPE_TIMER);
        VG_CHECK(!f.ctx.dispatchRuleEvent(owner));
        VG_CHECK(!f.ctx.file_channels.holdsTask());
        VG_CHECK(f.ctx.last_dispatched_owner.get() == owner);
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(!events[0].writer_created);
    };
    f.ctx.record_rule_enabled.store(false);
    rejected();
    f.ctx.record_rule_enabled.store(true);
    f.ctx.schedule_open.store(false);
    rejected();
    f.ctx.schedule_open.store(true);
    f.ctx.media_barrier.pause(false);
    rejected();
    f.ctx.media_barrier.resume();
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER))); // every gate open again
    VG_CHECK(f.ctx.file_channels.holdsTask());
    VG_CHECK_EQ(f.ctx.in_flight_budgets.inFlight(FakeSlotAdmission::kFilesystem),
        f.ctx.action_settings.get().settings->watermarks.task_bytes);
}

VG_TEST_CASE(continuous_admission_needs_no_owner_and_no_rule_flag_and_counts_as_no_rule_event)
{
    // docs/PLAN.md section 8.2 (Phase 5A-2): with continuous recording configured, dispatchContinuous admits a
    // UID-zero clip without an owner on the Video channel, against the schedule, the pause, and the Slot, not
    // the rule flag; nothing is reported, the last dispatched owner stays, and the triggered view rule info
    // reads ignores the clip. Triggered Video is suppressed meanwhile; Photo is not.
    Fixture f;
    f.ctx.tick(); // the shared clocks, which a clip's event time comes from
    auto &photo = f.ctx.file_channels.photo;
    auto &video = f.ctx.file_channels.video;
    Publish(f.ctx, ChannelSettings(f.ctx, CaptureType::Image), ChannelSettings(f.ctx, CaptureType::Video), true);
    const auto settings = f.ctx.action_settings.get().settings;
    VG_REQUIRE(settings->continuous != nullptr);

    // A rule event is admitted into Photo only, and reported as accepted.
    const auto owner = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner));
    VG_CHECK(photo.current() != nullptr && video.current() == nullptr);
    VG_CHECK_EQ(RuleEventRecorder::take().size(), size_t { 1 });

    // The continuous clip, admitted with rules disabled.
    f.ctx.record_rule_enabled.store(false);
    VG_CHECK(f.ctx.dispatchContinuous());
    const auto clip = video.current();
    VG_REQUIRE(clip != nullptr);
    VG_CHECK(clip->kind == FileTask::Kind::Continuous);
    VG_CHECK(clip->owner == nullptr);
    VG_CHECK(clip->settings == settings->continuous);
    VG_CHECK_EQ(clip->settings_revision, f.ctx.action_settings.channelRevision(CaptureType::Video));
    VG_CHECK_EQ(clip->event->owner_uid, uint64_t { 0 });
    VG_CHECK_EQ(clip->event->timestamp, f.ctx.shared_time.unixTime());
    VG_CHECK(clip->event->status.load() == WriteEvent::Status::Pending);
    VG_CHECK_EQ(clip->slot_registration, f.slot.registration());
    VG_CHECK_EQ(clip->reservation.bytes(), settings->continuous->watermarks.task_bytes);
    VG_CHECK(RuleEventRecorder::take().empty());
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner);
    VG_CHECK(f.ctx.file_channels.holdsTask());
    {
        const auto [task, held] = f.ctx.file_channels.latest(); // the triggered view: Photo's task
        VG_CHECK(task == photo.current() && held);
    }
    VG_CHECK(!f.ctx.dispatchContinuous()); // the channel holds a task

    // The disabled tick cancels the pending triggered task and leaves the clip.
    f.ctx.tick();
    VG_CHECK(photo.current() == nullptr);
    VG_CHECK(photo.last()->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(video.current() == clip && clip->event->status.load() == WriteEvent::Status::Pending);
    {
        const auto [task, held] = f.ctx.file_channels.latest();
        VG_CHECK(task == photo.last() && !held);
    }
    video.complete(clip, WriteEvent::Status::Completed);
    {
        const auto [task, held] = f.ctx.file_channels.latest(); // a completed clip is no rule event either
        VG_CHECK(task == photo.last() && !held);
    }

    // The gates of a continuous clip: the schedule, the pause, and a usable Slot with headroom.
    f.ctx.schedule_open.store(false);
    VG_CHECK(!f.ctx.dispatchContinuous());
    f.ctx.schedule_open.store(true);
    f.ctx.media_barrier.pause(false);
    VG_CHECK(!f.ctx.dispatchContinuous());
    f.ctx.media_barrier.resume();
    f.slot.ready = false;
    VG_CHECK(!f.ctx.dispatchContinuous());
    f.slot.ready = true;
    f.slot.free_bytes = settings->watermarks.critical_remain_bytes;
    VG_CHECK(!f.ctx.dispatchContinuous());
    f.slot.free_bytes = std::numeric_limits<FileSize>::max() / 2;
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    VG_CHECK(f.ctx.dispatchContinuous());
    VG_CHECK_EQ(f.ctx.in_flight_budgets.inFlight(FakeSlotAdmission::kFilesystem),
        settings->continuous->watermarks.task_bytes);
    video.cancelPending();

    // Without a Photo channel a rule event has no file action: it is reported with nothing accepted, and the
    // dispatch answers as a rule-only configuration does (docs/PLAN.md section 5.2).
    f.ctx.record_rule_enabled.store(true);
    Publish(f.ctx, nullptr, ChannelSettings(f.ctx, CaptureType::Video), true);
    VG_CHECK(!f.ctx.action_settings.get().settings->fileChannel());
    const auto owner2 = Owner(VG_RULE_TYPE_HTTP);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner2));
    VG_CHECK(!f.ctx.file_channels.holdsTask());
    VG_CHECK(f.ctx.last_dispatched_owner.get() == owner2);
    {
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(!events[0].writer_created);
    }
    Publish(f.ctx, nullptr, ChannelSettings(f.ctx, CaptureType::Video)); // continuous off: nothing to admit
    VG_CHECK(!f.ctx.dispatchContinuous());
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_TIMER))); // triggered Video accepts again
    VG_CHECK(video.current() != nullptr && video.current()->kind == FileTask::Kind::Triggered);
}

VG_TEST_CASE(the_continuation_gate_of_a_running_task_depends_on_its_kind)
{
    // docs/PLAN.md section 8.2: a triggered task runs while rules are enabled, a continuous clip while the
    // schedule is open, and either while the Video revision it was bound to is current, which a mode switch
    // moves, so a handover finishes the running clip of the old mode.
    Fixture f;
    const auto &video = f.ctx.file_channels.video;
    const auto rev = f.ctx.action_settings.channelRevision(CaptureType::Video);
    using Kind = FileTask::Kind;

    VG_CHECK(video.isOpenFor(Kind::Triggered, rev) && video.isOpenFor(Kind::Continuous, rev));
    f.ctx.record_rule_enabled.store(false);
    VG_CHECK(!video.isOpenFor(Kind::Triggered, rev) && video.isOpenFor(Kind::Continuous, rev));
    f.ctx.schedule_open.store(false);
    VG_CHECK(!video.isOpenFor(Kind::Triggered, rev) && !video.isOpenFor(Kind::Continuous, rev));
    f.ctx.record_rule_enabled.store(true);
    VG_CHECK(video.isOpenFor(Kind::Triggered, rev) && !video.isOpenFor(Kind::Continuous, rev));
    f.ctx.schedule_open.store(true);
    VG_CHECK(!video.isOpenFor(Kind::Triggered, rev + 1) && !video.isOpenFor(Kind::Continuous, rev + 1));

    // A mode switch moves the Video revision: the settings bound into a running task of either mode are stale.
    Publish(f.ctx, nullptr, ChannelSettings(f.ctx, CaptureType::Video), true);
    const auto continuous_rev = f.ctx.action_settings.channelRevision(CaptureType::Video);
    VG_CHECK(continuous_rev != rev);
    VG_CHECK(!video.isOpenFor(Kind::Triggered, rev) && !video.isOpenFor(Kind::Continuous, rev));
    VG_CHECK(video.isOpenFor(Kind::Continuous, continuous_rev));
    Publish(f.ctx, nullptr, ChannelSettings(f.ctx, CaptureType::Video));
    VG_CHECK(!video.isOpenFor(Kind::Continuous, continuous_rev));
    VG_CHECK(video.isOpenFor(Kind::Triggered, f.ctx.action_settings.channelRevision(CaptureType::Video)));
}

VG_TEST_CASE(source_less_owners_report_event_delivery)
{
    // Decided on 2026-10-19 (docs/PLAN.md section 6.1): owners without a source still carry EVENT delivery.
    Fixture f;
    const vg_rule_type_e types[] = { VG_RULE_TYPE_TIMER, VG_RULE_TYPE_HTTP, VG_RULE_TYPE_ALWAYS_ON, VG_RULE_TYPE_SED,
        VG_RULE_TYPE_GPIO, VG_RULE_TYPE_TTY };
    for (const auto type: types)
    {
        VG_CHECK(f.ctx.dispatchRuleEvent(Owner(type)));
        CompleteCurrent(f.ctx, WriteEvent::Status::Completed);
    }
    // The direct EVENT owner as the rule loop builds it, and a FRAME owner as an inference member builds it.
    VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_UNKNOWN, "", "cam0")));
    CompleteCurrent(f.ctx, WriteEvent::Status::Completed);
    VG_CHECK(f.ctx.dispatchRuleEvent(
        Owner(VG_RULE_TYPE_INFERENCE_SET, "person", "cam0", EventOwner::SourceDelivery::Frame)));

    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 8 });
    for (size_t i = 0; i < 6; ++i)
    {
        VG_CHECK_EQ(events[i].type, types[i]);
        VG_CHECK(events[i].writer_created);
        VG_CHECK_EQ(events[i].id, "");
        VG_CHECK_EQ(events[i].source_id, "");
        VG_CHECK_EQ(events[i].source_delivery, "EVENT");
        VG_CHECK(!events[i].id_is_null && !events[i].source_id_is_null && !events[i].source_delivery_is_null);
    }
    VG_CHECK_EQ(events[6].type, VG_RULE_TYPE_UNKNOWN);
    VG_CHECK_EQ(events[6].id, "");
    VG_CHECK_EQ(events[6].source_id, "cam0");
    VG_CHECK_EQ(events[6].source_delivery, "EVENT");
    VG_CHECK_EQ(events[7].type, VG_RULE_TYPE_INFERENCE_SET);
    VG_CHECK_EQ(events[7].id, "person");
    VG_CHECK_EQ(events[7].source_id, "cam0");
    VG_CHECK_EQ(events[7].source_delivery, "FRAME");

    VG_CHECK_EQ(std::string(EventOwner::SourceDeliveryString(EventOwner::SourceDelivery::Unknown)), "");
}

VG_TEST_CASE(timer_rule_paces_on_the_context_clock)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    // The timer takes its reference time from the real clock at construction, but evaluation reads the
    // context's steady time, which only moves on tick(). A rule constructed after the last tick is
    // therefore not eligible until the next tick, whatever its interval.
    const TimerRule untimed(TimerRuleConfig(nlohmann::json { { "iIntervalSeconds", 0 } }, f.ctx), f.ctx);
    const TimerRule paced(TimerRuleConfig(nlohmann::json { { "iIntervalSeconds", 1 } }, f.ctx), f.ctx);
    VG_CHECK(untimed(f.ctx) == nullptr);
    VG_CHECK(paced(f.ctx) == nullptr);

    f.ctx.tick();
    const auto owner = untimed(f.ctx);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->type, VG_RULE_TYPE_TIMER);
    VG_CHECK_EQ(owner->id, "");
    VG_CHECK_EQ(owner->source_id, "");
    VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Event);
    VG_CHECK(untimed(f.ctx) != nullptr); // interval zero: every evaluation

    VG_CHECK(paced(f.ctx) != nullptr); // the first interval is already elapsed at construction
    VG_CHECK(paced(f.ctx) == nullptr); // same tick: the interval has not elapsed again

    VG_CHECK_THROWS(TimerRuleConfig(nlohmann::json { { "iIntervalSeconds", -1 } }, f.ctx));
}

VG_TEST_CASE(always_on_waits_for_the_current_event_and_for_media)
{
    Fixture f;
    f.ctx.record_rule_enabled.store(true);
    f.ctx.tick();

    {
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx); // eligible immediately after construction
        const auto owner = rule(f.ctx);
        VG_REQUIRE(owner != nullptr);
        VG_CHECK_EQ(owner->type, VG_RULE_TYPE_ALWAYS_ON);
        VG_CHECK_EQ(owner->id, "");
        VG_CHECK_EQ(owner->source_id, "");
        VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Event);
        VG_CHECK(rule(f.ctx) == nullptr); // the 5000 ms default interval has not elapsed on this tick
    }
    {
        VG_CHECK(f.ctx.dispatchRuleEvent(Owner(VG_RULE_TYPE_HTTP)));
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx);
        VG_CHECK(rule(f.ctx) == nullptr); // a pending task blocks it without consuming its interval
        const auto task = f.ctx.file_channels.video.claim(f.slot.registration());
        VG_REQUIRE(task != nullptr);
        VG_CHECK(rule(f.ctx) == nullptr); // a running one too
        // The write finished: the worker holds the channel through the interval, and so does the rule.
        task->event->status.store(WriteEvent::Status::Completed);
        VG_CHECK(rule(f.ctx) == nullptr);
        f.ctx.file_channels.video.complete(task, WriteEvent::Status::Completed);
        VG_CHECK(rule(f.ctx) != nullptr);
    }
    {
        f.ctx.media_barrier.pause(false);
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx);
        VG_CHECK(rule(f.ctx) == nullptr);
        f.ctx.media_barrier.resume();
        {
            const Context::MediaBarrier::Operation operation(f.ctx.media_barrier); // a media operation in flight
            VG_CHECK(rule(f.ctx) == nullptr);
        }
        VG_CHECK(rule(f.ctx) != nullptr);
    }
    {
        // Without a usable Slot each interval's dispatch is rejected and reported, and the interval is consumed
        // as before: the pacing is unchanged by admission at dispatch (docs/PLAN.md sections 4.4 and 8.1).
        f.slot.ready = false;
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx);
        const auto owner = rule(f.ctx);
        VG_REQUIRE(owner != nullptr);
        VG_CHECK(!f.ctx.dispatchRuleEvent(owner));
        VG_CHECK(!f.ctx.file_channels.holdsTask());
        VG_CHECK(rule(f.ctx) == nullptr);
        f.slot.ready = true;
    }
}

VG_TEST_CASE(media_barrier_refuses_admission_while_paused_and_a_waiting_pause_drains)
{
    Fixture f;
    auto &barrier = f.ctx.media_barrier;
    VG_CHECK(!barrier.isPaused());
    VG_CHECK_EQ(barrier.activeOperations(), size_t { 0 });

    // Admission is refused while paused; an operation admitted earlier stays counted until it leaves.
    std::optional<Context::MediaBarrier::Operation> admitted;
    admitted.emplace(barrier);
    VG_CHECK(static_cast<bool>(*admitted));
    VG_CHECK_EQ(barrier.activeOperations(), size_t { 1 });
    barrier.pause(false);
    VG_CHECK(barrier.isPaused());
    VG_CHECK_EQ(barrier.activeOperations(), size_t { 1 }); // a non-waiting pause does not drain
    {
        const Context::MediaBarrier::Operation refused(barrier);
        VG_CHECK(!refused);
        VG_CHECK_EQ(barrier.activeOperations(), size_t { 1 }); // a refused operation is not counted
    }
    barrier.resume();
    VG_CHECK(!barrier.isPaused());

    // A waiting pause closes admission first and returns only once the admitted operation has left.
    std::atomic<bool> drained = false;
    std::thread pauser([&] {
        barrier.pause(true);
        drained.store(true);
    });
    while (!barrier.isPaused())
        std::this_thread::yield();
    VG_CHECK(!drained.load()); // cannot have returned: the operation admitted above is still active
    {
        const Context::MediaBarrier::Operation refused(barrier);
        VG_CHECK(!refused); // admission is closed before the drain completes
    }
    admitted.reset();
    pauser.join();
    VG_CHECK(drained.load());
    VG_CHECK_EQ(barrier.activeOperations(), size_t { 0 });
    barrier.resume();
    VG_CHECK(static_cast<bool>(Context::MediaBarrier::Operation(barrier)));
}

VG_TEST_CASE(in_flight_budgets_admit_against_the_free_space_minus_reservations_and_release)
{
    // docs/PLAN.md section 9: an admission counts what earlier admissions may still write to the same
    // filesystem, reserves its own bytes in the same critical section, and releases them when its holder goes.
    Fixture f;
    auto &budgets = f.ctx.in_flight_budgets;
    constexpr Context::InFlightBudgets::FilesystemId fs = 7, other = 8;
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 0 });
    auto a = budgets.tryReserve(fs, 100, /* free */ 1000, /* critical */ 500);
    VG_CHECK(static_cast<bool>(a));
    VG_CHECK_EQ(a.bytes(), FileSize { 100 });
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 100 });
    auto b = budgets.tryReserve(fs, 300, 1000, 500); // 1000 - 100 is above the mark
    VG_CHECK(static_cast<bool>(b));
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 400 });
    VG_CHECK(!budgets.tryReserve(fs, 50, 1000, 600)); // 1000 - 400 is not: refused, nothing reserved
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 400 });
    VG_CHECK(static_cast<bool>(budgets.tryReserve(other, 50, 1000, 600))); // another filesystem is independent
    VG_CHECK_EQ(budgets.inFlight(other), FileSize { 0 });                  // and that one was released at once
    a.release();
    VG_CHECK(!a);
    VG_CHECK_EQ(a.bytes(), FileSize { 0 });
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 300 });
    a.release(); // idempotent
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 300 });
    {
        const auto moved = std::move(b); // a moved reservation is released once, by its new holder
        VG_CHECK(!b && static_cast<bool>(moved));
        VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 300 });
    }
    VG_CHECK_EQ(budgets.inFlight(fs), FileSize { 0 });
    VG_CHECK(static_cast<bool>(budgets.tryReserve(fs, 50, 1000, 600))); // it was the reservations, not the space
}

VG_TEST_CASE(owners_carry_an_event_time_and_a_dispatch_time)
{
    const auto before = EventOwner::NowMs();
    const EventOwner observed(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event);
    const EventOwner from_input(VG_RULE_TYPE_UNKNOWN, "", "cam0", EventOwner::SourceDelivery::Event, int64_t { 4242 });
    const auto after = EventOwner::NowMs();

    // An owner that observes its condition itself: the event time is the dispatch time.
    VG_CHECK(observed.dispatch_timestamp >= before && observed.dispatch_timestamp <= after);
    VG_CHECK_EQ(observed.timestamp, observed.dispatch_timestamp);
    // An owner built from an input keeps the input's time as its event time and still records the dispatch.
    VG_CHECK_EQ(from_input.timestamp, int64_t { 4242 });
    VG_CHECK(from_input.dispatch_timestamp >= before && from_input.dispatch_timestamp <= after);
    VG_CHECK(from_input.uid > observed.uid);
}

// Last on purpose: UIDs are process-global and only ever increase.
VG_TEST_CASE(uids_are_unique_increasing_and_exact_above_2_to_53)
{
    const EventOwner a(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, int64_t { 100 });
    const EventOwner b(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, int64_t { 100 });
    VG_CHECK(b.uid > a.uid); // same timestamp, distinct identity

    const auto big = (uint64_t { 1 } << 53) + 1; // not representable as a double
    const EventOwner c(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, static_cast<int64_t>(big));
    VG_CHECK_EQ(c.uid, big);

    nlohmann::json j;
    j["iUID"] = c.uid;
    VG_CHECK_EQ(j.dump(), R"({"iUID":9007199254740993})");
    VG_CHECK_EQ(j["iUID"].get<uint64_t>(), big);

    const EventOwner d(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, int64_t { 100 });
    VG_CHECK_EQ(d.uid, big + 1); // never goes backwards, even for an older timestamp
}

VG_TEST_MAIN()
