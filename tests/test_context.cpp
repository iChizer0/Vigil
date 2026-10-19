// Characterization tests for Context: inference intake, the per-tick input hand-off, rule event
// creation/reporting, and the owner identity that the C event projection is built from
// (docs/PLAN.md sections 5.1, 6.1, 6.2; matrix R1 and W1). Everything here is today's behavior.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/rules.hpp"
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::rules;
using namespace vigil::tests;

namespace {

struct Fixture
{
    Context ctx;

    Fixture() : ctx(MakeFakeConfig())
    {
        RuleEventRecorder::clear();
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
    f.ctx.tick();
    VG_CHECK(f.ctx.shared_time.steadyTime() > before); // the clocks advance on an enabled tick
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
    VG_CHECK(f.ctx.tryCreateWriterEvent(owner));
    VG_CHECK(f.ctx.shared_rule_event.getCurrent() != nullptr);

    f.ctx.record_rule_enabled.store(false);
    f.ctx.tick();
    VG_CHECK_EQ(f.ctx.inference_queue.size(), size_t { 0 });
    VG_CHECK(!f.ctx.shared_inference_result.has_value());
    VG_CHECK(f.ctx.shared_rule_event.getCurrent() == nullptr);
    const auto last = f.ctx.shared_rule_event.getLast();
    VG_REQUIRE(last != nullptr);
    VG_CHECK(last->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK_EQ(last->owner_uid, owner->uid);
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
    const auto owner1
        = std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event, int64_t { 5000 });
    VG_CHECK(f.ctx.tryCreateWriterEvent(owner1));
    const auto current = f.ctx.shared_rule_event.getCurrent();
    VG_REQUIRE(current != nullptr);
    VG_CHECK_EQ(current->owner_uid, owner1->uid);
    VG_CHECK(current->status.load() == WriteEvent::Status::Pending);
    VG_CHECK_EQ(current->timestamp, f.ctx.shared_time.unixTime()); // the context clock, not the owner's time

    // A second owner while one event is pending: reported, but no new writer event.
    const auto owner2
        = std::make_shared<EventOwner>(VG_RULE_TYPE_HTTP, "", "", EventOwner::SourceDelivery::Event, int64_t { 6000 });
    VG_CHECK(!f.ctx.tryCreateWriterEvent(owner2));
    VG_CHECK_EQ(f.ctx.shared_rule_event.getCurrent()->owner_uid, owner1->uid);

    const auto events = RuleEventRecorder::take();
    VG_REQUIRE(events.size() == size_t { 2 });
    VG_CHECK_EQ(events[0].type, VG_RULE_TYPE_TIMER);
    VG_CHECK(events[0].writer_created);
    VG_CHECK_EQ(events[0].uid, owner1->uid);
    VG_CHECK_EQ(events[0].timestamp, int64_t { 5000 });
    VG_CHECK(!events[0].id_is_null); // borrowed strings are empty, never null
    VG_CHECK_EQ(events[0].id, "");
    VG_CHECK(!events[0].source_id_is_null);
    VG_CHECK_EQ(events[0].source_id, "");
    VG_CHECK(!events[0].source_delivery_is_null);
    VG_CHECK_EQ(events[0].source_delivery, "EVENT");
    VG_CHECK_EQ(events[1].type, VG_RULE_TYPE_HTTP);
    VG_CHECK(!events[1].writer_created);
    VG_CHECK_EQ(events[1].uid, owner2->uid);

    // Completing the event frees the slot and keeps the previous owner as "last".
    f.ctx.shared_rule_event.reset(WriteEvent::Status::Completed);
    VG_CHECK(f.ctx.shared_rule_event.getCurrent() == nullptr);
    const auto last = f.ctx.shared_rule_event.getLast();
    VG_REQUIRE(last != nullptr);
    VG_CHECK(last->status.load() == WriteEvent::Status::Completed);
    VG_CHECK_EQ(last->owner_uid, owner1->uid);
    const auto [last_evt, last_owner, current_evt, current_owner] = f.ctx.shared_rule_event.get();
    VG_CHECK(last_evt == last);
    VG_CHECK(last_owner == owner1);
    VG_CHECK(current_evt == nullptr);
    VG_CHECK(current_owner == nullptr);

    const auto owner3 = Owner(VG_RULE_TYPE_TIMER);
    VG_CHECK(f.ctx.tryCreateWriterEvent(owner3));
    VG_CHECK_EQ(f.ctx.shared_rule_event.getCurrent()->owner_uid, owner3->uid);

    // A null owner is ignored without a callback.
    VG_CHECK(!f.ctx.tryCreateWriterEvent(nullptr));
    VG_CHECK_EQ(RuleEventRecorder::size(), size_t { 1 });
}

VG_TEST_CASE(source_less_owners_report_event_delivery)
{
    // Decided on 2026-10-19 (docs/PLAN.md section 6.1): owners without a source still carry EVENT delivery.
    Fixture f;
    const vg_rule_type_e types[] = { VG_RULE_TYPE_TIMER, VG_RULE_TYPE_HTTP, VG_RULE_TYPE_ALWAYS_ON, VG_RULE_TYPE_SED,
        VG_RULE_TYPE_GPIO, VG_RULE_TYPE_TTY };
    for (const auto type: types)
    {
        VG_CHECK(f.ctx.tryCreateWriterEvent(Owner(type)));
        f.ctx.shared_rule_event.reset(WriteEvent::Status::Completed);
    }
    // The direct EVENT owner as the rule loop builds it, and a FRAME owner as an inference member builds it.
    VG_CHECK(f.ctx.tryCreateWriterEvent(Owner(VG_RULE_TYPE_UNKNOWN, "", "cam0")));
    f.ctx.shared_rule_event.reset(WriteEvent::Status::Completed);
    VG_CHECK(f.ctx.tryCreateWriterEvent(
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
        VG_CHECK(f.ctx.tryCreateWriterEvent(Owner(VG_RULE_TYPE_HTTP)));
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx);
        VG_CHECK(rule(f.ctx) == nullptr); // a pending writer event blocks it without consuming its interval
        f.ctx.shared_rule_event.reset(WriteEvent::Status::Completed);
        VG_CHECK(rule(f.ctx) != nullptr);
    }
    {
        f.ctx.media_paused.store(true);
        const AlwaysOnRule rule(AlwaysOnRuleConfig {}, f.ctx);
        VG_CHECK(rule(f.ctx) == nullptr);
        f.ctx.media_paused.store(false);
        f.ctx.media_writers_active.store(1);
        VG_CHECK(rule(f.ctx) == nullptr);
        f.ctx.media_writers_active.store(0);
        VG_CHECK(rule(f.ctx) != nullptr);
    }
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
