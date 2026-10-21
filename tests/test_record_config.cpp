// Characterization tests for the version-2 record action configuration (docs/PLAN.md section 4.1; matrix
// C1) and for the action settings snapshot, its revisions, and the watermarks it is mapped onto (section 7.1;
// matrix A4 and C2).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"
#include "fake_slot.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::tests;

namespace {

struct Fixture
{
    Context ctx;
    FakeSlotAdmission slot; // registered and ready: a dispatch needs a usable Slot (docs/PLAN.md section 8.1)

    Fixture() : ctx(MakeFakeConfig()), slot(ctx) { }
};

using Overrides = std::initializer_list<std::pair<const char *, nlohmann::json>>;

// The first-start document in the shape GET returns, with `overrides` applied by JSON pointer.
nlohmann::json Document(Overrides overrides = {})
{
    auto j = nlohmann::json::parse(R"({
        "iSchemaVersion": 2,
        "bRuleEnabled": false,
        "bContinuousRecordEnabled": false,
        "dActions": {
            "dPhoto": { "bEnabled": false, "sFormat": "JPG", "iIntervalMs": 5000 },
            "dVideo": { "bEnabled": false, "iIntervalMs": 5000 },
            "dSound": { "bEnabled": false }
        }
    })");
    for (const auto &[pointer, value]: overrides)
        j[nlohmann::json::json_pointer(pointer)] = value;
    return j;
}

nlohmann::json Canonical(const RecordConfig &cfg, const Context &ctx)
{
    nlohmann::json j;
    cfg.to(j, ctx);
    return j;
}

// True when the document is rejected; an accepted document is printed so that a failed check names it.
bool Rejected(const nlohmann::json &j, const Context &ctx)
{
    try
    {
        const RecordConfig cfg(j, ctx);
    }
    catch (const std::exception &)
    {
        return true;
    }
    std::fprintf(stderr, "accepted: %s\n", j.dump().c_str());
    return false;
}

int64_t EstimateOneMiB(const char *, size_t, void *)
{
    return 1024 * 1024;
}

int64_t EstimateNothing(const char *, size_t, void *)
{
    return 0;
}

// The host's estimate for the refresh test, changed between calls.
int64_t g_estimate = 0;

int64_t EstimateFromGlobal(const char *, size_t, void *)
{
    return g_estimate;
}

// The enabled file channel of the published snapshot, null without one.
std::shared_ptr<FileWriterSettings> Channel(const Context &ctx)
{
    const auto settings = ctx.action_settings.get().settings;
    return settings ? settings->fileChannel() : nullptr;
}

// The snapshot of the first-start document with `overrides` applied.
std::shared_ptr<const ActionSettings> Snapshot(const Context &ctx, Overrides overrides = {})
{
    return RecordConfig(Document(overrides), ctx).prepare(ctx).settings;
}

} // namespace

VG_TEST_CASE(defaults_are_the_first_start_document)
{
    Fixture f;
    const RecordConfig cfg;
    VG_CHECK(!cfg.rule_enabled);
    VG_CHECK(!cfg.continuous_enabled);
    VG_CHECK(!cfg.photo.enabled && !cfg.video.enabled && !cfg.sound.enabled);
    VG_CHECK_EQ(Canonical(cfg, f.ctx), Document());
    const auto settings = cfg.prepare(f.ctx).settings;
    VG_REQUIRE(settings != nullptr);
    VG_CHECK(!settings->fileChannel() && !settings->continuous && !settings->sound_enabled);
    VG_CHECK_EQ(Canonical(RecordConfig(Document(), f.ctx), f.ctx), Document()); // and it round-trips
}

VG_TEST_CASE(parses_a_complete_document_and_returns_it_canonically)
{
    Fixture f;
    const auto doc = Document(
        { { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dPhoto/sFormat", "RAW" },
            { "/dActions/dPhoto/iIntervalMs", 1500 }, { "/dActions/dVideo/iIntervalMs", 2500 } });
    const RecordConfig cfg(doc, f.ctx);
    VG_CHECK(cfg.rule_enabled);
    VG_CHECK(!cfg.continuous_enabled);
    VG_CHECK(cfg.photo.enabled);
    VG_CHECK_EQ(cfg.photo.format, "RAW");
    VG_CHECK_EQ(cfg.photo.interval.count(), 1500L);
    VG_CHECK(!cfg.video.enabled);
    VG_CHECK_EQ(cfg.video.interval.count(), 2500L);
    VG_CHECK(!cfg.sound.enabled);
    VG_CHECK_EQ(Canonical(cfg, f.ctx), doc);

    // Unknown fields are ignored and dropped from the canonical document.
    auto extra = doc;
    extra["sNote"] = "dropped";
    extra["dActions"]["dPhoto"]["iExtra"] = 1;
    VG_CHECK_EQ(Canonical(RecordConfig(extra, f.ctx), f.ctx), doc);
}

VG_TEST_CASE(rejects_other_schemas_incomplete_documents_and_wrong_types)
{
    Fixture f;
    VG_CHECK(!Rejected(Document(), f.ctx));
    for (const auto &[pointer, value]: Overrides {
             { "/iSchemaVersion", 1 },
             { "/iSchemaVersion", 2.0 }, // numerically equal, but not an integer
             { "/iSchemaVersion", "2" },
             { "/iSchemaVersion", nullptr },
             { "/bRuleEnabled", "yes" },
             { "/bRuleEnabled", 1 },
             { "/bContinuousRecordEnabled", 0 },
             { "/dActions", nlohmann::json::array() },
             { "/dActions/dPhoto", "JPG" },
             { "/dActions/dPhoto/sFormat", "MP4" },
             { "/dActions/dPhoto/sFormat", "jpg" },
             { "/dActions/dPhoto/sFormat", 1 },
             { "/dActions/dPhoto/iIntervalMs", 0 }, // pinned 2026-10-19 (docs/PLAN.md section 2): not "no gap"
             { "/dActions/dVideo/iIntervalMs", -1 },
             { "/dActions/dVideo/iIntervalMs", 1.5 },
             { "/dActions/dVideo/iIntervalMs", "5000" },
             { "/dActions/dVideo/iIntervalMs", uint64_t { 1 } << 63 }, // not representable as int64
             { "/dActions/dSound/bEnabled", "false" },
         })
        VG_CHECK(Rejected(Document({ { pointer, value } }), f.ctx));

    // Every field is required: a document missing any of them is rejected rather than defaulted.
    for (const char *pointer: { "/iSchemaVersion", "/bRuleEnabled", "/bContinuousRecordEnabled", "/dActions",
             "/dActions/dPhoto", "/dActions/dPhoto/sFormat", "/dActions/dVideo/iIntervalMs", "/dActions/dSound" })
    {
        auto doc = Document();
        const nlohmann::json::json_pointer p(pointer);
        doc[p.parent_pointer()].erase(p.back());
        VG_CHECK(Rejected(doc, f.ctx));
    }
    // The retired version-1 shape is one such document; nothing is imported from it.
    VG_CHECK(Rejected(
        nlohmann::json::parse(R"({"bRuleEnabled":true,"dWriterConfig":{"sFormat":"MP4","iIntervalMs":5000}})"), f.ctx));
    VG_CHECK(Rejected(nlohmann::json::array(), f.ctx));
    VG_CHECK(Rejected(nlohmann::json(nullptr), f.ctx));
}

VG_TEST_CASE(accepts_every_action_combination_and_continuous_recording)
{
    // The multi-action configuration (docs/PLAN.md section 4.1, Phases 4D and 5A-3): any combination of actions,
    // with or without rules enabled, round-trips canonically and maps onto the snapshot, and so does continuous
    // recording, with or without a Video action, which it suppresses for rule events (section 8.2).
    Fixture f;
    for (const auto &doc: { Document({ { "/bContinuousRecordEnabled", true } }),
             Document({ { "/bContinuousRecordEnabled", true }, { "/bRuleEnabled", true },
                 { "/dActions/dVideo/bEnabled", true } }) })
    {
        const RecordConfig cfg(doc, f.ctx);
        VG_CHECK(cfg.continuous_enabled);
        VG_CHECK_EQ(Canonical(cfg, f.ctx), doc);
        const auto s = cfg.prepare(f.ctx).settings;
        VG_REQUIRE(s != nullptr && s->continuous != nullptr);
        VG_CHECK(!s->channel(CaptureType::Video) && !s->fileChannel()); // triggered Video suppressed
        VG_CHECK_EQ(s->video != nullptr, cfg.video.enabled);
        VG_CHECK(s->videoRecording() == s->continuous);
    }

    const auto all = Document(
        { { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dPhoto/sFormat", "RAW" },
            { "/dActions/dPhoto/iIntervalMs", 1500 }, { "/dActions/dVideo/bEnabled", true },
            { "/dActions/dVideo/iIntervalMs", 2500 }, { "/dActions/dSound/bEnabled", true } });
    const RecordConfig every(all, f.ctx);
    VG_CHECK(every.rule_enabled && every.photo.enabled && every.video.enabled && every.sound.enabled);
    VG_CHECK_EQ(Canonical(every, f.ctx), all);
    const auto settings = every.prepare(f.ctx).settings;
    VG_REQUIRE(settings != nullptr && settings->photo != nullptr && settings->video != nullptr);
    VG_CHECK(settings->sound_enabled && !settings->continuous);
    VG_CHECK_EQ(settings->photo->format, "RAW");
    VG_CHECK_EQ(settings->photo->interval.count(), 1500L);
    VG_CHECK_EQ(settings->video->interval.count(), 2500L);
    VG_CHECK(settings->watermarks == WaterMarks::aggregate(settings->photo->watermarks, settings->video->watermarks));
    VG_CHECK(settings->fileChannel() == settings->photo);

    // Sound alone, with or without rules, and rules enabled with nothing to do: every rule event is reported
    // and nothing else happens.
    for (const auto &doc: { Document({ { "/dActions/dSound/bEnabled", true } }),
             Document({ { "/bRuleEnabled", true }, { "/dActions/dSound/bEnabled", true } }),
             Document({ { "/bRuleEnabled", true } }) })
    {
        const RecordConfig cfg(doc, f.ctx);
        VG_CHECK_EQ(Canonical(cfg, f.ctx), doc);
        const auto s = cfg.prepare(f.ctx).settings;
        VG_REQUIRE(s != nullptr);
        VG_CHECK(!s->fileChannel());
        VG_CHECK_EQ(s->sound_enabled, cfg.sound.enabled);
    }
}

VG_TEST_CASE(the_always_on_invariant_admits_one_channel_and_exactly_one_while_rules_are_enabled)
{
    // docs/PLAN.md section 4.4: with ALWAYS_ON selected, at most one file channel, Sound disabled, continuous
    // disabled, and exactly one channel while rules are enabled; the first-start state satisfies it.
    Fixture f;
    const auto satisfies
        = [&](Overrides overrides) { return RecordConfig(Document(overrides), f.ctx).satisfiesAlwaysOnInvariant(); };
    VG_CHECK(RecordConfig().satisfiesAlwaysOnInvariant());
    VG_CHECK(satisfies({}));
    VG_CHECK(satisfies({ { "/dActions/dPhoto/bEnabled", true } }));
    VG_CHECK(satisfies({ { "/dActions/dVideo/bEnabled", true } }));
    VG_CHECK(satisfies({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true } }));
    VG_CHECK(satisfies({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }));
    VG_CHECK(!satisfies({ { "/bRuleEnabled", true } })); // rules on: exactly one channel
    VG_CHECK(!satisfies({ { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dVideo/bEnabled", true } }));
    VG_CHECK(!satisfies(
        { { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dVideo/bEnabled", true } }));
    VG_CHECK(!satisfies({ { "/dActions/dSound/bEnabled", true } }));
    VG_CHECK(!satisfies(
        { { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true }, { "/dActions/dSound/bEnabled", true } }));
    VG_CHECK(!satisfies({ { "/bContinuousRecordEnabled", true } })); // rules disabled or not
    VG_CHECK(!satisfies(
        { { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true }, { "/bContinuousRecordEnabled", true } }));
}

VG_TEST_CASE(action_settings_carry_the_enabled_file_channel_and_the_effective_watermarks)
{
    Fixture f;
    const auto photo = Snapshot(f.ctx, { { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dPhoto/sFormat", "RAW" },
                                           { "/dActions/dPhoto/iIntervalMs", 1500 } });
    VG_REQUIRE(photo != nullptr && photo->photo != nullptr);
    VG_CHECK(photo->video == nullptr);
    VG_CHECK(photo->fileChannel() == photo->photo);
    VG_CHECK(photo->photo->type == CaptureType::Image);
    VG_CHECK_EQ(photo->photo->format, "RAW");
    VG_CHECK_EQ(photo->photo->interval.count(), 1500L);
    VG_CHECK_EQ(photo->photo->file_name_prefix, kRecordWriterPrefixDefault);
    VG_CHECK_EQ(photo->photo->video_clip_length.count(), kVideoClipLengthDefault.count());
    VG_CHECK(photo->photo->watermarks == WaterMarks::from("RAW", 1, nullptr));
    VG_CHECK(photo->watermarks == photo->photo->watermarks); // the effective watermarks are the channel's
    VG_CHECK(!photo->continuous && !photo->sound_enabled);

    const auto video
        = Snapshot(f.ctx, { { "/dActions/dVideo/bEnabled", true }, { "/dActions/dVideo/iIntervalMs", 2500 } });
    VG_REQUIRE(video != nullptr && video->video != nullptr);
    VG_CHECK(video->photo == nullptr);
    VG_CHECK(video->fileChannel() == video->video);
    VG_CHECK(video->video->type == CaptureType::Video);
    VG_CHECK_EQ(video->video->format, "MP4");
    VG_CHECK_EQ(video->video->interval.count(), 2500L);
    VG_CHECK(video->video->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), nullptr));
    VG_CHECK(video->watermarks == video->video->watermarks);

    // No file channel: the effective watermarks are those of one default clip, the first-start headroom.
    const auto none = Snapshot(f.ctx);
    VG_REQUIRE(none != nullptr);
    VG_CHECK(!none->fileChannel());
    VG_CHECK(
        none->watermarks == WaterMarks::from(kRecordWriterDefaultFormat, kVideoClipLengthDefault.count(), nullptr));

    // Both channels: the effective watermarks are the aggregate of one Photo plus one Video task (docs/PLAN.md
    // section 9).
    const auto both = ActionSettings::create(false, false, photo->photo, video->video, &f.ctx.c_api.media);
    VG_REQUIRE(both != nullptr);
    const auto &pw = photo->photo->watermarks, &vw = video->video->watermarks;
    VG_CHECK(both->watermarks == WaterMarks::aggregate(pw, vw));
    VG_CHECK_EQ(both->watermarks.task_bytes, pw.task_bytes + vw.task_bytes);
    VG_CHECK_EQ(both->watermarks.critical_remain_bytes,
        static_cast<FileSize>(kStorageCriticalWaterMarkFactor) * both->watermarks.task_bytes
            + static_cast<FileSize>(kStorageMinFileSystemFreeBytes));
    VG_CHECK_EQ(both->watermarks.high_remain_bytes,
        static_cast<FileSize>(kStorageHighWaterMarkFactor) * both->watermarks.task_bytes
            + static_cast<FileSize>(kStorageMinFileSystemFreeBytes));
    VG_CHECK(both->watermarks.critical_remain_bytes > vw.critical_remain_bytes);

    // Snapshots compare by content.
    VG_CHECK(*none == *Snapshot(f.ctx));
    VG_CHECK(!(*photo == *video) && !(*video == *none));
}

VG_TEST_CASE(action_settings_carry_the_continuous_video_settings_as_the_video_recording)
{
    // docs/PLAN.md sections 7.1, 8.2, and 9 (Phase 5A-2): a snapshot with continuous recording configured
    // carries the continuous Video settings (MP4, no interval, the default clip length, one clip's
    // watermarks), suppresses triggered Video for rule events without rewriting it, and budgets one Video
    // task for either mode. Built directly here; the parsed document is pinned above.
    Fixture f;
    const auto photo = Snapshot(f.ctx, { { "/dActions/dPhoto/bEnabled", true } })->photo;
    const auto video = Snapshot(f.ctx, { { "/dActions/dVideo/bEnabled", true } })->video;
    VG_REQUIRE(photo != nullptr && video != nullptr);

    const auto both = ActionSettings::create(true, false, photo, video, &f.ctx.c_api.media);
    VG_REQUIRE(both != nullptr && both->continuous != nullptr);
    VG_CHECK(both->continuous->type == CaptureType::Video);
    VG_CHECK_EQ(both->continuous->format, "MP4");
    VG_CHECK_EQ(both->continuous->interval.count(), 0L);
    VG_CHECK_EQ(both->continuous->file_name_prefix, kRecordWriterPrefixDefault);
    VG_CHECK_EQ(both->continuous->video_clip_length.count(), kVideoClipLengthDefault.count());
    VG_CHECK(both->continuous->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), nullptr));
    VG_CHECK(both->video == video); // configured, and reported as such
    VG_CHECK(both->configured(CaptureType::Video) == video);
    VG_CHECK(both->channel(CaptureType::Video) == nullptr); // suppressed for rule events
    VG_CHECK(both->channel(CaptureType::Image) == photo);
    VG_CHECK(both->videoRecording() == both->continuous);
    VG_CHECK(both->fileChannel() == photo);
    VG_CHECK(both->watermarks == WaterMarks::aggregate(photo->watermarks, both->continuous->watermarks));

    // Without Photo the continuous clip is the only task budgeted, and a rule event has no file action.
    const auto alone = ActionSettings::create(true, false, nullptr, video, &f.ctx.c_api.media);
    VG_REQUIRE(alone != nullptr);
    VG_CHECK(!alone->fileChannel());
    VG_CHECK(alone->watermarks == alone->continuous->watermarks);
    const auto unconfigured = ActionSettings::create(true, false, nullptr, nullptr, &f.ctx.c_api.media);
    VG_REQUIRE(unconfigured != nullptr);
    VG_CHECK(!unconfigured->video && unconfigured->videoRecording() == unconfigured->continuous);
    VG_CHECK(!unconfigured->configured(CaptureType::Video) && !unconfigured->fileChannel());

    // Without continuous recording the triggered Video settings are the recording.
    const auto triggered = ActionSettings::create(false, false, nullptr, video, &f.ctx.c_api.media);
    VG_REQUIRE(triggered != nullptr);
    VG_CHECK(!triggered->continuous && triggered->videoRecording() == video);
    VG_CHECK(triggered->channel(CaptureType::Video) == video && triggered->fileChannel() == video);

    // Snapshots compare by content, the continuous settings included; a refresh re-estimates them too.
    VG_CHECK(*both == *ActionSettings::create(true, false, photo, video, &f.ctx.c_api.media));
    VG_CHECK(!(*both == *ActionSettings::create(false, false, photo, video, &f.ctx.c_api.media)));
    auto media = f.ctx.c_api.media;
    media.max_bytes_estimate_cb = &EstimateOneMiB;
    const auto refreshed = both->refreshed(&media);
    VG_REQUIRE(refreshed != nullptr && refreshed->continuous != nullptr);
    VG_CHECK(refreshed->continuous->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), &media));
    VG_CHECK(!(refreshed->continuous->watermarks == both->continuous->watermarks));
    VG_CHECK(refreshed->watermarks
             == WaterMarks::aggregate(refreshed->photo->watermarks, refreshed->continuous->watermarks));
}

VG_TEST_CASE(the_video_revision_follows_the_video_recording_of_either_mode)
{
    // docs/PLAN.md sections 7.1 and 8.2: the Video revision moves when the recording the Video channel runs
    // changes: a mode switch moves it, a change to the suppressed triggered Video settings does not, and the
    // Photo revision is unaffected either way. Through RecordConfig::sync, a switch cancels the pending task
    // of the old mode, and the clip of the new mode is admitted afterwards.
    Fixture f;
    auto &shared = f.ctx.action_settings;
    auto &video = f.ctx.file_channels.video;
    const auto publish = [&](bool continuous, int video_interval_ms) {
        const auto settings = Snapshot(f.ctx,
            { { "/dActions/dVideo/bEnabled", true }, { "/dActions/dVideo/iIntervalMs", video_interval_ms } });
        VG_REQUIRE(settings != nullptr);
        VG_REQUIRE(shared.update(shared.getRevision(),
            ActionSettings::create(continuous, false, settings->photo, settings->video, &f.ctx.c_api.media)));
    };

    publish(false, 2500);
    auto before = shared.get();
    publish(true, 2500); // the switch to continuous: the recording changed
    auto after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    before = after;
    publish(true, 3000); // the suppressed triggered interval changed: the snapshot moved, the recording did not
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    before = after;
    publish(false, 3000); // the switch back to triggered Video
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    // Through sync: the switch to continuous cancels the pending triggered task, the continuous clip is
    // admitted, the same configuration again keeps it, and the switch back cancels it while pending.
    const RecordConfig triggered(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), f.ctx);
    const RecordConfig continuous(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true },
                                      { "/bContinuousRecordEnabled", true } }),
        f.ctx);
    VG_CHECK_EQ(triggered.sync(f.ctx, triggered.prepare(f.ctx)), 0);
    VG_CHECK(f.ctx.dispatchRuleEvent(
        std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event)));
    const auto pending = video.current();
    VG_REQUIRE(pending != nullptr);
    VG_CHECK(!f.ctx.dispatchContinuous()); // not configured
    VG_CHECK_EQ(continuous.sync(f.ctx, continuous.prepare(f.ctx)), 0);
    VG_CHECK(video.current() == nullptr && pending->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(f.ctx.dispatchContinuous());
    const auto clip = video.current();
    VG_REQUIRE(clip != nullptr && clip->kind == FileTask::Kind::Continuous);
    VG_CHECK_EQ(continuous.sync(f.ctx, continuous.prepare(f.ctx)), 0); // the same configuration: nothing published
    VG_CHECK(video.current() == clip);
    VG_CHECK_EQ(triggered.sync(f.ctx, triggered.prepare(f.ctx)), 0); // the switch back
    VG_CHECK(video.current() == nullptr && clip->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(!f.ctx.dispatchContinuous());
}

VG_TEST_CASE(sync_publishes_the_snapshot_and_the_rule_flag)
{
    Fixture f;
    const auto rev0 = f.ctx.action_settings.getRevision();
    const RecordConfig photo(Document({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true } }), f.ctx);
    VG_CHECK_EQ(photo.sync(f.ctx, photo.prepare(f.ctx)), 0);
    VG_CHECK(f.ctx.record_rule_enabled.load());
    VG_REQUIRE(Channel(f.ctx) != nullptr);
    VG_CHECK_EQ(Channel(f.ctx)->format, "JPG");
    VG_CHECK_EQ(f.ctx.action_settings.getRevision(), rev0 + 1);

    // No enabled file action: the flag is published and the snapshot has no file channel.
    const RecordConfig none(Document(), f.ctx);
    VG_CHECK_EQ(none.sync(f.ctx, none.prepare(f.ctx)), 0);
    VG_CHECK(!f.ctx.record_rule_enabled.load());
    VG_CHECK(Channel(f.ctx) == nullptr);
    VG_CHECK_EQ(f.ctx.action_settings.getRevision(), rev0 + 2);

    // The same document again publishes nothing.
    VG_CHECK_EQ(none.sync(f.ctx, none.prepare(f.ctx)), 0);
    VG_CHECK_EQ(f.ctx.action_settings.getRevision(), rev0 + 2);
}

VG_TEST_CASE(sync_cancels_the_pending_task_of_a_channel_whose_settings_changed)
{
    // A pending task is bound to the channel settings of the snapshot it was accepted under (docs/PLAN.md
    // section 7.1): publishing different settings for that channel cancels it, the same settings keep it, and a
    // claimed task is left to its worker, which stops on the revision change.
    Fixture f;
    auto &video = f.ctx.file_channels.video;
    const auto owner
        = [] { return std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event); };
    const RecordConfig cfg(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), f.ctx);
    VG_CHECK_EQ(cfg.sync(f.ctx, cfg.prepare(f.ctx)), 0);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner()));
    const auto pending = video.current();
    VG_REQUIRE(pending != nullptr);

    VG_CHECK_EQ(cfg.sync(f.ctx, cfg.prepare(f.ctx)), 0); // the same document: nothing published, the task kept
    VG_CHECK(video.current() == pending);
    VG_CHECK(pending->event->status.load() == WriteEvent::Status::Pending);

    const RecordConfig changed(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true },
                                   { "/dActions/dVideo/iIntervalMs", 2500 } }),
        f.ctx);
    VG_CHECK_EQ(changed.sync(f.ctx, changed.prepare(f.ctx)), 0);
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(video.last() == pending);
    VG_CHECK(pending->event->status.load() == WriteEvent::Status::Canceled);
    VG_CHECK(f.ctx.record_rule_enabled.load());

    VG_CHECK(f.ctx.dispatchRuleEvent(owner())); // accepted again, with the new settings
    const auto claimed = video.claim(f.slot.registration());
    VG_REQUIRE(claimed != nullptr);
    VG_CHECK_EQ(claimed->settings->interval.count(), 2500L);
    VG_CHECK_EQ(cfg.sync(f.ctx, cfg.prepare(f.ctx)), 0); // back to 5000 ms: the claimed task is its worker's
    VG_CHECK(video.current() == claimed);
    VG_CHECK(claimed->event->status.load() == WriteEvent::Status::Writing);
    VG_CHECK(f.ctx.action_settings.channelRevision(CaptureType::Video) != claimed->settings_revision); // stops it
}

VG_TEST_CASE(cancel_pending_keeps_a_task_bound_to_the_kept_revision)
{
    // The cancel predicate: RecordConfig::sync cancels the pending task after the publish and outside the settings
    // mutex, so a dispatcher can accept a task under the new settings in between, and the cancel keeps the task
    // bound to the revision the commit published while it retires one bound to another. Without a revision, as at
    // teardown, it cancels whatever is pending. The commit itself is pinned by the next case.
    Fixture f;
    auto &video = f.ctx.file_channels.video;
    const auto owner
        = [] { return std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event); };
    const RecordConfig cfg(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), f.ctx);
    VG_CHECK_EQ(cfg.sync(f.ctx, cfg.prepare(f.ctx)), 0);
    VG_CHECK(f.ctx.dispatchRuleEvent(owner()));
    const auto accepted = video.current();
    VG_REQUIRE(accepted != nullptr);
    const auto published = f.ctx.action_settings.channelRevision(CaptureType::Video);
    VG_CHECK_EQ(accepted->settings_revision, published);

    video.cancelPending(published); // the commit that published these settings
    VG_CHECK(video.current() == accepted);
    VG_CHECK(accepted->event->status.load() == WriteEvent::Status::Pending);

    video.cancelPending(published + 1); // a later commit
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(accepted->event->status.load() == WriteEvent::Status::Canceled);

    VG_CHECK(f.ctx.dispatchRuleEvent(owner()));
    const auto again = video.current();
    VG_REQUIRE(again != nullptr);
    video.cancelPending(); // teardown
    VG_CHECK(video.current() == nullptr);
    VG_CHECK(again->event->status.load() == WriteEvent::Status::Canceled);
}

VG_TEST_CASE(a_commit_keeps_a_task_accepted_under_the_settings_it_published)
{
    // The commit, deterministically: RecordConfig::sync publishes, then cancels on the Photo channel before the
    // Video channel, each cancel taking its channel's lock. A Photo admission that blocks inside the Slot's probe
    // holds the Photo channel lock, so the commit waits there after its publish; a continuous clip accepted on
    // the Video channel meanwhile is bound to the Video revision the commit published, and the commit's Video
    // cancel, once the admission lets go, must keep it. The case relies on the Photo cancel preceding the Video
    // one in sync: swapped, the commit would pass before the clip is accepted and the case would pin nothing.
    struct BlockingAdmission: FakeSlotAdmission
    {
        using FakeSlotAdmission::FakeSlotAdmission;
        std::atomic<bool> block_next = false, blocked = false, release = false;

        Context::InFlightBudgets::Reservation tryAdmit(const WaterMarks &admission,
            FileSize task_bytes) noexcept override
        {
            if (block_next.exchange(false))
            {
                blocked = true;
                while (!release.load())
                    std::this_thread::yield();
            }
            return FakeSlotAdmission::tryAdmit(admission, task_bytes);
        }
    };
    const auto wait_for = [](const auto &pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    };

    Context ctx(MakeFakeConfig());
    BlockingAdmission slot(ctx);
    auto &photo = ctx.file_channels.photo;
    auto &video = ctx.file_channels.video;
    const RecordConfig before(Document({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true } }), ctx);
    const RecordConfig after(Document({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true },
                                 { "/dActions/dPhoto/iIntervalMs", 2500 }, { "/bContinuousRecordEnabled", true } }),
        ctx);
    VG_REQUIRE(before.sync(ctx, before.prepare(ctx)) == 0);

    std::thread admission, commit;
    struct Unblock // lets the admission go and joins both threads, also when a required check throws
    {
        BlockingAdmission &slot;
        std::thread &admission, &commit;
        ~Unblock()
        {
            slot.release = true;
            for (auto *const thread: { &admission, &commit })
                if (thread->joinable())
                    thread->join();
        }
    } unblock { slot, admission, commit };

    // A Photo admission under the old settings, held inside the Slot's probe with the Photo channel lock taken.
    slot.block_next = true;
    const auto owner = std::make_shared<EventOwner>(VG_RULE_TYPE_TIMER, "", "", EventOwner::SourceDelivery::Event);
    std::shared_ptr<FileTask> photo_task;
    admission = std::thread([&] { photo_task = photo.tryAccept(owner, ctx.action_settings.get()); });
    VG_REQUIRE(wait_for([&] { return slot.blocked.load(); }));

    // The commit publishes, then waits for the Photo channel lock before its Video cancel.
    const auto revision = ctx.action_settings.getRevision();
    const auto prepared = after.prepare(ctx);
    vg_sta_e committed = -1;
    commit = std::thread([&] { committed = after.sync(ctx, prepared); });
    VG_REQUIRE(wait_for([&] { return ctx.action_settings.getRevision() != revision; }));

    // Between the publish and the Video cancel: a continuous clip under the published Video revision.
    const auto clip = video.tryAcceptContinuous(ctx.action_settings.get());
    VG_REQUIRE(clip != nullptr);
    VG_CHECK_EQ(clip->settings_revision, ctx.action_settings.channelRevision(CaptureType::Video));
    slot.release = true;
    admission.join();
    commit.join();
    VG_CHECK_EQ(committed, 0);
    VG_REQUIRE(photo_task != nullptr);
    VG_CHECK(photo_task->event->status.load() == WriteEvent::Status::Canceled); // bound to the old Photo settings
    VG_CHECK(video.current() == clip); // kept: bound to the revision the commit published
    VG_CHECK(clip->event->status.load() == WriteEvent::Status::Pending);
    ctx.file_channels.cancelPending();
}

VG_TEST_CASE(default_action_settings_exist_before_any_configuration)
{
    Fixture f;
    const auto loaded = f.ctx.action_settings.get();
    VG_REQUIRE(loaded.settings != nullptr);
    VG_CHECK(!loaded.settings->fileChannel());
    VG_CHECK(!loaded.settings->continuous && !loaded.settings->sound_enabled);
    VG_CHECK(loaded.settings->watermarks
             == WaterMarks::from(kRecordWriterDefaultFormat, kVideoClipLengthDefault.count(), nullptr));
    VG_CHECK(!f.ctx.record_rule_enabled.load());
}

VG_TEST_CASE(publishing_bumps_only_the_revisions_of_the_channels_that_changed)
{
    Fixture f;
    auto &shared = f.ctx.action_settings;
    const auto publish = [&](std::shared_ptr<const ActionSettings> next) {
        VG_REQUIRE(next != nullptr);
        return shared.update(shared.getRevision(), std::move(next));
    };
    const auto video = [&](int interval_ms) {
        return Snapshot(f.ctx,
            { { "/dActions/dVideo/bEnabled", true }, { "/dActions/dVideo/iIntervalMs", interval_ms } });
    };
    const auto photo = [&](int interval_ms) {
        return Snapshot(f.ctx,
            { { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dPhoto/iIntervalMs", interval_ms } });
    };

    // Enabling Video moves the Video revision; the Photo channel was and stays disabled.
    auto before = shared.get();
    VG_CHECK(publish(video(2500)));
    auto after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);
    VG_CHECK_EQ(shared.channelRevision(CaptureType::Video), after.video_revision);
    VG_CHECK_EQ(shared.channelRevision(CaptureType::Image), after.photo_revision);

    // An equal snapshot publishes nothing, so a re-posted configuration stops no writer.
    before = after;
    VG_CHECK(publish(video(2500)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    // A Video interval change moves the Video revision only.
    before = after;
    VG_CHECK(publish(video(3000)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    // A Sound switch leaves both file channels alone.
    before = after;
    VG_CHECK(publish(
        ActionSettings::create(false, true, before.settings->photo, before.settings->video, &f.ctx.c_api.media)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK(after.settings->sound_enabled);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    // A change of a channel's watermarks alone (what a media refresh does) moves the global revision and the
    // snapshot, which admission reads, but not the channel revision a running task is bound to (docs/PLAN.md
    // section 9, decided 2026-10-19).
    before = after;
    {
        const auto &v = *before.settings->video;
        VG_CHECK(publish(ActionSettings::create(false, true, before.settings->photo,
            FileWriterSettings::create(v.type, v.format, v.interval, v.file_name_prefix, v.video_clip_length,
                WaterMarks::from(v.format, 2 * kVideoClipLengthDefault.count(), nullptr)),
            &f.ctx.c_api.media)));
    }
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK(!(after.settings->video->watermarks == before.settings->video->watermarks));
    VG_CHECK(after.settings->watermarks == after.settings->video->watermarks);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);

    // Switching the file action from Video to Photo changes both channels.
    before = after;
    VG_CHECK(publish(photo(1500)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision + 1);

    // A Photo interval change moves the Photo revision only.
    before = after;
    VG_CHECK(publish(photo(2000)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision);

    // Disabling every action retires the Photo channel.
    before = after;
    VG_CHECK(publish(Snapshot(f.ctx)));
    after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK(!after.settings->fileChannel());
}

VG_TEST_CASE(a_stale_revision_is_refused_and_applies_nothing)
{
    Fixture f;
    auto &shared = f.ctx.action_settings;
    const auto base = shared.get();
    const auto video = Snapshot(f.ctx, { { "/dActions/dVideo/bEnabled", true } });
    const auto photo = Snapshot(f.ctx, { { "/dActions/dPhoto/bEnabled", true } });
    VG_REQUIRE(video != nullptr && photo != nullptr);

    // A commit that read the revision before another commit (or a media refresh) landed is refused with
    // nothing applied; RecordConfig::sync reports it as EAGAIN and the client posts again.
    VG_CHECK(shared.update(base.revision, video));
    VG_CHECK(!shared.update(base.revision, photo));
    const auto current = shared.get();
    VG_CHECK_EQ(current.revision, base.revision + 1);
    VG_CHECK(current.settings->video != nullptr && current.settings->photo == nullptr);
    VG_CHECK_EQ(current.photo_revision, base.photo_revision);
    VG_CHECK(!shared.update(current.revision + 1, photo)); // a revision from the future is no better
    VG_CHECK_EQ(shared.get().revision, current.revision);

    // Retrying from the current revision succeeds.
    VG_CHECK(shared.update(current.revision, photo));
    VG_CHECK(shared.get().settings->photo != nullptr);
    VG_CHECK_EQ(shared.get().revision, current.revision + 1);
}

VG_TEST_CASE(a_commit_prepared_before_a_media_refresh_is_refused)
{
    // The snapshot is prepared outside the commit lock with the host's estimate of that moment; a refresh that
    // lands before the publish changed the base it was built on, so the publish is refused and the client
    // prepares again. Until the review fix of 2026-10-19 the revision was read at publish time, and the stale
    // snapshot silently replaced the refreshed one.
    auto cfg = MakeFakeConfig();
    cfg.media.max_bytes_estimate_cb = &EstimateFromGlobal;
    g_estimate = 0;
    Context ctx(cfg);
    auto &shared = ctx.action_settings;
    const auto *media = &ctx.c_api.media;

    const RecordConfig video(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), ctx);
    const auto prepared = video.prepare(ctx);
    VG_REQUIRE(prepared.settings != nullptr);
    g_estimate = 1024 * 1024;
    shared.sync(*media); // the refresh lands between prepare and publish
    const auto refreshed = shared.get();
    VG_CHECK_EQ(video.sync(ctx, prepared), EAGAIN);
    VG_CHECK(!ctx.record_rule_enabled.load()); // nothing applied
    VG_CHECK_EQ(shared.get().revision, refreshed.revision);
    VG_CHECK(shared.get().settings->video == nullptr);

    // Prepared again, the commit carries the refreshed estimate.
    VG_CHECK_EQ(video.sync(ctx, video.prepare(ctx)), 0);
    VG_CHECK(ctx.record_rule_enabled.load());
    VG_REQUIRE(shared.get().settings->video != nullptr);
    VG_CHECK(
        shared.get().settings->video->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), media));
}

VG_TEST_CASE(a_media_refresh_re_estimates_every_channel_coherently)
{
    auto cfg = MakeFakeConfig();
    cfg.media.max_bytes_estimate_cb = &EstimateFromGlobal;
    g_estimate = 0; // the table values apply
    Context ctx(cfg);
    auto &shared = ctx.action_settings;
    const auto *media = &ctx.c_api.media;

    const RecordConfig video(
        Document({ { "/dActions/dVideo/bEnabled", true }, { "/dActions/dVideo/iIntervalMs", 2500 } }), ctx);
    VG_CHECK_EQ(video.sync(ctx, video.prepare(ctx)), 0);
    const auto before = shared.get();
    VG_REQUIRE(before.settings != nullptr && before.settings->video != nullptr);
    VG_CHECK(before.settings->video->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), nullptr));

    // The host now estimates one clip at 1 MiB: the channel's and the effective watermarks change together in
    // one snapshot. No channel revision moves (since Phase 4C-3 a running task keeps its own watermark; before,
    // a refresh interrupted it), and the disabled Photo channel is untouched.
    g_estimate = 1024 * 1024;
    shared.sync(*media);
    const auto after = shared.get();
    VG_CHECK_EQ(after.revision, before.revision + 1);
    VG_CHECK_EQ(after.video_revision, before.video_revision);
    VG_CHECK_EQ(after.photo_revision, before.photo_revision);
    VG_REQUIRE(after.settings->video != nullptr);
    VG_CHECK(after.settings->video->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), media));
    VG_CHECK(!(after.settings->video->watermarks == before.settings->video->watermarks));
    VG_CHECK(after.settings->watermarks == after.settings->video->watermarks);
    VG_CHECK_EQ(after.settings->video->interval.count(), 2500L); // everything else is kept
    VG_CHECK(after.settings->photo == nullptr);

    // The same estimate again changes nothing.
    shared.sync(*media);
    VG_CHECK_EQ(shared.get().revision, after.revision);
    VG_CHECK_EQ(shared.get().video_revision, after.video_revision);

    // Without a file channel the refresh re-estimates the default clip's watermarks; no channel revision moves.
    const RecordConfig none(Document(), ctx);
    VG_CHECK_EQ(none.sync(ctx, none.prepare(ctx)), 0);
    const auto idle = shared.get();
    g_estimate = 2 * 1024 * 1024;
    shared.sync(*media);
    const auto refreshed = shared.get();
    VG_CHECK_EQ(refreshed.revision, idle.revision + 1);
    VG_CHECK_EQ(refreshed.photo_revision, idle.photo_revision);
    VG_CHECK_EQ(refreshed.video_revision, idle.video_revision);
    VG_CHECK(refreshed.settings->watermarks
             == WaterMarks::from(kRecordWriterDefaultFormat, kVideoClipLengthDefault.count(), media));
    VG_CHECK(!(refreshed.settings->watermarks == idle.settings->watermarks));
}

VG_TEST_CASE(watermarks_follow_the_format_table_unless_the_host_estimates)
{
    constexpr FileSize kMiB = 1024 * 1024;
    constexpr FileSize kMinFree = static_cast<FileSize>(kStorageMinFileSystemFreeBytes);

    const auto jpg = WaterMarks::from("JPG", 1, nullptr); // 10 MiB per image
    VG_CHECK_EQ(jpg.task_bytes, 10 * kMiB);
    VG_CHECK_EQ(jpg.critical_remain_bytes, 2 * 10 * kMiB + kMinFree);
    VG_CHECK_EQ(jpg.high_remain_bytes, 5 * 10 * kMiB + kMinFree);
    VG_CHECK_EQ(jpg.recycle_bytes, 10 * 10 * kMiB + kMinFree);
    VG_CHECK(WaterMarks::from("JPG", 0, nullptr) == jpg); // batch is at least one

    const auto mp4 = WaterMarks::from("MP4", 60, nullptr); // 3 MiB per second, one 60-second clip
    VG_CHECK_EQ(mp4.task_bytes, 180 * kMiB);
    VG_CHECK_EQ(mp4.critical_remain_bytes, 2 * 180 * kMiB + kMinFree);
    VG_CHECK_EQ(mp4.high_remain_bytes, 5 * 180 * kMiB + kMinFree);

    // An unknown format uses the fallback entry (it read past the table before 2026-10-19; latent, since
    // every caller validates the format first).
    const auto unknown = WaterMarks::from("AVI", 1, nullptr);
    VG_CHECK(unknown == WaterMarks::from("", 1, nullptr));
    VG_CHECK_EQ(unknown.critical_remain_bytes, 3 * kMinFree);
    VG_CHECK_EQ(unknown.high_remain_bytes, 6 * kMinFree);

    vg_config_media_t media {};
    media.max_bytes_estimate_cb = &EstimateOneMiB;
    const auto estimated = WaterMarks::from("MP4", 60, &media);
    VG_CHECK_EQ(estimated.task_bytes, kMiB);
    VG_CHECK_EQ(estimated.critical_remain_bytes, 2 * kMiB + kMinFree);
    media.max_bytes_estimate_cb = &EstimateNothing; // a non-positive estimate keeps the table value
    VG_CHECK(WaterMarks::from("MP4", 60, &media) == mp4);
}

VG_TEST_MAIN()
