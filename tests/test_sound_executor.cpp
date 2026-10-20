// Characterization tests for the Sound action executor (docs/PLAN.md section 7.3; matrix S1 and A5): one
// state, no queue, no retry; a pending trigger is canceled by rule disable, Sound disable, pause, and stop; a
// playing callback holds a media operation and finishes before a waiting pause or the stop returns. Most cases
// drive the executor without its worker thread; two run the real worker with a callback that blocks until the
// test releases it.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_slot.hpp"

namespace vigil {

struct SoundActionExecutorTestAccess
{
    static bool playPending(Context::SoundActionExecutor &executor)
    {
        return executor.playPending();
    }
};

} // namespace vigil

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::tests;

namespace {

using State = Context::SoundActionExecutor::State;

// The host's playback callback: records every call and what it observed from inside, and optionally blocks
// until the test releases it.
struct FakeSound
{
    Context *ctx = nullptr;
    vg_sta_e result = 0;
    std::atomic<bool> block = false;
    std::atomic<int> calls = 0;
    std::atomic<int> finished = 0;
    std::atomic<size_t> active_while_playing = 0;
    std::atomic<int> state_while_playing = -1;
    std::atomic<bool> retrigger_rejected = false;
    std::mutex mutex;
    std::condition_variable released_cv;
    bool released = false;

    static vg_sta_e Play(void *user_data)
    {
        auto &s = *static_cast<FakeSound *>(user_data);
        ++s.calls;
        s.active_while_playing = s.ctx->media_barrier.activeOperations();
        s.state_while_playing = static_cast<int>(s.ctx->sound_executor.state());
        s.retrigger_rejected = !s.ctx->sound_executor.tryTrigger(); // a Playing retrigger is rejected
        if (s.block)
        {
            std::unique_lock lock(s.mutex);
            s.released_cv.wait(lock, [&s] { return s.released; });
        }
        ++s.finished;
        return s.result;
    }

    void release()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        released_cv.notify_all();
    }
};

// Counts the executor's playback failure lines on top of the shared test log callback.
struct ErrorLog
{
    static inline std::atomic<int> playback_failures = 0;

    static vg_sta_e log(vg_log_level_e level, const char *msg, size_t len)
    {
        if (level == VG_LOG_LEVEL_ERROR && msg
            && std::string(msg, len).find("Sound playback failed") != std::string::npos)
            ++playback_failures;
        return TestLogCallback(level, msg, len);
    }
};

struct Fixture
{
    FakeSound sound;
    Context ctx;
    FakeSlotAdmission slot; // registered and ready: the dispatch case needs a usable Slot for its file task

    explicit Fixture(bool with_callback = true, bool run_worker = false)
        : ctx(Config(sound, with_callback), run_worker), slot(ctx)
    {
        sound.ctx = &ctx;
        ErrorLog::playback_failures = 0;
        RuleEventRecorder::clear();
    }

    static vg_config_t Config(FakeSound &sound, bool with_callback)
    {
        auto cfg = MakeFakeConfig();
        cfg.basic.log_cb = &ErrorLog::log;
        cfg.media.user_data = &sound;
        if (with_callback)
            cfg.media.sound_play_cb = &FakeSound::Play;
        return cfg;
    }

    Context::SoundActionExecutor &executor()
    {
        return ctx.sound_executor;
    }

    // Plays the pending trigger on this thread, as the worker would.
    bool serve()
    {
        return SoundActionExecutorTestAccess::playPending(ctx.sound_executor);
    }

    // Spins until `pred` holds: a bounded wait on the worker, not a timing assumption.
    template<typename Pred>
    static bool waitFor(Pred pred)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    // Publishes a snapshot with the Video channel enabled and Sound switched as given, built directly: the
    // parser rejects Sound until Phase 4D.
    void publishSound(bool enabled)
    {
        auto &shared = ctx.action_settings;
        VG_REQUIRE(shared.update(shared.getRevision(),
            ActionSettings::create(false, enabled, nullptr,
                FileWriterSettings::create(CaptureType::Video, kRecordWriterDefaultFormat,
                    kRecordWriterDefaultIntervalMs, kRecordWriterPrefixDefault, kVideoClipLengthDefault,
                    &ctx.c_api.media),
                &ctx.c_api.media)));
    }

    // Enables Sound and rules, the gates the worker rechecks after claiming a trigger.
    void arm()
    {
        publishSound(true);
        ctx.record_rule_enabled.store(true);
    }
};

std::shared_ptr<EventOwner> Owner()
{
    return std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event);
}

} // namespace

VG_TEST_CASE(without_a_callback_the_sound_action_is_unavailable)
{
    Fixture f(/* with_callback = */ false, /* run_worker = */ true);
    VG_CHECK(!f.executor().available());
    VG_CHECK(!f.executor().tryTrigger());
    VG_CHECK(f.executor().state() == State::Idle);
    VG_CHECK(!f.serve());
    VG_CHECK_EQ(f.sound.calls.load(), 0);
    f.executor().stopAndJoin(); // no worker to join; stays stopped
    f.executor().stopAndJoin();
    VG_CHECK(f.executor().state() == State::Stopping);
    VG_CHECK(!f.executor().tryTrigger());
}

VG_TEST_CASE(a_trigger_is_accepted_from_idle_only_and_played_once)
{
    Fixture f;
    f.arm();
    VG_CHECK(f.executor().available());
    VG_CHECK(f.executor().tryTrigger());
    VG_CHECK(f.executor().state() == State::Pending);
    VG_CHECK(!f.executor().tryTrigger()); // a Pending retrigger is rejected, not queued
    VG_CHECK(f.serve());
    VG_CHECK_EQ(f.sound.calls.load(), 1);
    VG_CHECK_EQ(f.sound.state_while_playing.load(), static_cast<int>(State::Playing));
    VG_CHECK(f.sound.retrigger_rejected.load());                    // and so is a Playing retrigger
    VG_CHECK_EQ(f.sound.active_while_playing.load(), size_t { 1 }); // the callback runs under a media operation
    VG_CHECK(f.executor().state() == State::Idle);
    VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
    VG_CHECK(!f.serve()); // nothing was queued behind the rejected retriggers
    VG_CHECK_EQ(f.sound.calls.load(), 1);
    VG_CHECK(f.executor().tryTrigger()); // Idle again
}

VG_TEST_CASE(a_failed_callback_is_logged_once_and_never_retried)
{
    Fixture f;
    f.arm();
    f.sound.result = EIO;
    VG_CHECK(f.executor().tryTrigger());
    VG_CHECK(f.serve());
    VG_CHECK_EQ(f.sound.calls.load(), 1);
    VG_CHECK_EQ(ErrorLog::playback_failures.load(), 1);
    VG_CHECK(f.executor().state() == State::Idle); // no cooldown, no backlog
    VG_CHECK(!f.serve());
    VG_CHECK_EQ(f.sound.calls.load(), 1);
    VG_CHECK(f.executor().tryTrigger()); // the next trigger is a new decision
}

VG_TEST_CASE(a_pending_trigger_is_canceled_by_rule_disable_sound_disable_and_stop)
{
    {
        Fixture f;
        VG_CHECK(f.executor().tryTrigger());
        f.executor().cancelPending();
        VG_CHECK(f.executor().state() == State::Idle);
        VG_CHECK(!f.serve());
        VG_CHECK_EQ(f.sound.calls.load(), 0);
    }
    {
        Fixture f; // rules disabled: the tick cancels the trigger like the pending file task
        VG_CHECK(f.executor().tryTrigger());
        f.ctx.tick();
        VG_CHECK(f.executor().state() == State::Idle);
        VG_CHECK(!f.serve());
    }
    {
        Fixture f; // a configuration that disables Sound
        VG_CHECK(f.executor().tryTrigger());
        const RecordConfig none(nlohmann::json::parse(R"({
            "iSchemaVersion": 2, "bRuleEnabled": false, "bContinuousRecordEnabled": false,
            "dActions": { "dPhoto": { "bEnabled": false, "sFormat": "JPG", "iIntervalMs": 5000 },
                          "dVideo": { "bEnabled": false, "iIntervalMs": 5000 }, "dSound": { "bEnabled": false } }
        })"),
            f.ctx);
        VG_CHECK_EQ(none.sync(f.ctx, none.prepare(f.ctx)), 0);
        VG_CHECK(f.executor().state() == State::Idle);
        VG_CHECK(!f.serve());
    }
    {
        Fixture f; // stop
        VG_CHECK(f.executor().tryTrigger());
        f.executor().stopAndJoin();
        VG_CHECK(f.executor().state() == State::Stopping);
        VG_CHECK(!f.serve());
        VG_CHECK(!f.executor().tryTrigger());
        VG_CHECK_EQ(f.sound.calls.load(), 0);
    }
}

VG_TEST_CASE(a_pause_refuses_triggers_and_admission_and_a_waiting_pause_drains_playback)
{
    {
        Fixture f;
        f.ctx.media_barrier.pause(false);
        VG_CHECK(!f.executor().tryTrigger()); // refused at the trigger
        f.ctx.media_barrier.resume();
        VG_CHECK(f.executor().tryTrigger());
        f.ctx.media_barrier.pause(false); // paused between the trigger and the worker's claim
        VG_CHECK(f.serve());              // claimed, but admission was refused: canceled, nothing played
        VG_CHECK_EQ(f.sound.calls.load(), 0);
        VG_CHECK(f.executor().state() == State::Idle);
        f.ctx.media_barrier.resume();
    }
    {
        Fixture f(/* with_callback = */ true, /* run_worker = */ true);
        f.arm();
        f.sound.block = true;
        VG_CHECK(f.executor().tryTrigger());
        VG_REQUIRE(Fixture::waitFor([&] { return f.sound.calls.load() == 1; }));
        VG_CHECK(f.executor().state() == State::Playing);
        // A waiting pause returns only once the playing callback has returned.
        std::atomic<int> finished_at_pause_return = -1;
        std::thread pauser([&] {
            f.ctx.media_barrier.pause(true);
            finished_at_pause_return = f.sound.finished.load();
        });
        VG_REQUIRE(Fixture::waitFor([&] { return f.ctx.media_barrier.isPaused(); }));
        VG_CHECK(!f.executor().tryTrigger()); // playing, and paused
        f.sound.release();
        pauser.join();
        VG_CHECK_EQ(finished_at_pause_return.load(), 1);
        VG_REQUIRE(Fixture::waitFor([&] { return f.executor().state() == State::Idle; }));
        VG_CHECK_EQ(f.ctx.media_barrier.activeOperations(), size_t { 0 });
        f.ctx.media_barrier.resume();
        f.executor().stopAndJoin();
    }
}

VG_TEST_CASE(a_claimed_trigger_is_canceled_when_a_gate_closed_since_the_trigger)
{
    // The dispatcher loads the snapshot before it triggers, so a commit can disable Sound or rules in between
    // without finding anything pending to cancel; the worker rechecks both after claiming.
    {
        Fixture f;
        f.arm();
        VG_CHECK(f.executor().tryTrigger());
        f.publishSound(false);
        VG_CHECK(f.serve());
        VG_CHECK_EQ(f.sound.calls.load(), 0);
        VG_CHECK(f.executor().state() == State::Idle);
    }
    {
        Fixture f;
        f.arm();
        VG_CHECK(f.executor().tryTrigger());
        f.ctx.record_rule_enabled.store(false);
        VG_CHECK(f.serve());
        VG_CHECK_EQ(f.sound.calls.load(), 0);
        VG_CHECK(f.executor().state() == State::Idle);
    }
}

VG_TEST_CASE(stop_waits_for_the_playing_callback_and_the_executor_stays_stopped)
{
    Fixture f(/* with_callback = */ true, /* run_worker = */ true);
    f.arm();
    f.sound.block = true;
    VG_CHECK(f.executor().tryTrigger());
    VG_REQUIRE(Fixture::waitFor([&] { return f.sound.calls.load() == 1; }));
    std::atomic<int> finished_at_stop_return = -1;
    std::thread stopper([&] {
        f.executor().stopAndJoin();
        finished_at_stop_return = f.sound.finished.load();
    });
    VG_CHECK(f.executor().state() == State::Playing); // a stop request leaves the callback to finish
    f.sound.release();
    stopper.join();
    VG_CHECK_EQ(finished_at_stop_return.load(), 1);
    VG_CHECK(f.executor().state() == State::Stopping);
    VG_CHECK(!f.executor().tryTrigger());
    f.executor().stopAndJoin(); // idempotent
    VG_CHECK_EQ(f.sound.calls.load(), 1);
}

VG_TEST_CASE(dispatch_triggers_sound_only_when_the_snapshot_enables_it_and_reports_the_dispatch_time)
{
    Fixture f;
    f.publishSound(false); // one Video channel, Sound disabled
    f.ctx.record_rule_enabled.store(true);
    f.ctx.tick();
    const auto owner = Owner();
    VG_CHECK(f.ctx.dispatchRuleEvent(owner)); // the file task is accepted; Sound disabled: nothing is triggered
    VG_CHECK(f.executor().state() == State::Idle);
    {
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(events[0].writer_created);
        VG_CHECK_EQ(events[0].timestamp, owner->timestamp);
        VG_CHECK_EQ(events[0].dispatch_timestamp, owner->dispatch_timestamp);
    }

    f.publishSound(true);
    VG_CHECK(!f.ctx.dispatchRuleEvent(Owner()));      // the file task is still pending: not accepted again
    VG_CHECK(f.executor().state() == State::Pending); // but Sound was triggered on its own
    {
        const auto events = RuleEventRecorder::take();
        VG_REQUIRE(events.size() == size_t { 1 });
        VG_CHECK(!events[0].writer_created); // file acceptance only; Sound is not part of it
    }
    VG_CHECK(!f.ctx.dispatchRuleEvent(Owner())); // a third dispatch keeps the one Pending trigger, no queue
    VG_CHECK(f.executor().state() == State::Pending);
    VG_CHECK(f.serve());
    VG_CHECK_EQ(f.sound.calls.load(), 1);
    VG_CHECK(!f.serve());
}

VG_TEST_MAIN()
