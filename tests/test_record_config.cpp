// Characterization tests for the version-2 record action configuration (docs/PLAN.md section 4.1; matrix
// C1) and for the shared writer settings and watermarks it is mapped onto.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <string>
#include <utility>

#include "vigil/configs.hpp"
#include "vigil/constants.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::constants;
using namespace vigil::tests;

namespace {

struct Fixture
{
    Context ctx;

    Fixture() : ctx(MakeFakeConfig()) { }
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

} // namespace

VG_TEST_CASE(defaults_are_the_first_start_document)
{
    Fixture f;
    const RecordConfig cfg;
    VG_CHECK(!cfg.rule_enabled);
    VG_CHECK(!cfg.continuous_enabled);
    VG_CHECK(!cfg.photo.enabled && !cfg.video.enabled && !cfg.sound.enabled);
    VG_CHECK_EQ(Canonical(cfg, f.ctx), Document());
    VG_CHECK(cfg.createWriterSettings(f.ctx) == nullptr);
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

VG_TEST_CASE(rejects_capabilities_the_single_writer_runtime_cannot_run_yet)
{
    Fixture f;
    VG_CHECK(Rejected(Document({ { "/bContinuousRecordEnabled", true } }), f.ctx));
    VG_CHECK(Rejected(Document({ { "/dActions/dSound/bEnabled", true } }), f.ctx));
    VG_CHECK(
        Rejected(Document({ { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), f.ctx));
    VG_CHECK(Rejected(Document({ { "/bRuleEnabled", true } }), f.ctx)); // rules on with nothing to do
    VG_CHECK(!Rejected(Document({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true } }), f.ctx));
    VG_CHECK(!Rejected(Document({ { "/bRuleEnabled", true }, { "/dActions/dVideo/bEnabled", true } }), f.ctx));
    VG_CHECK(!Rejected(Document({ { "/dActions/dVideo/bEnabled", true } }), f.ctx)); // rules off, action armed
}

VG_TEST_CASE(writer_settings_follow_the_single_enabled_file_action)
{
    Fixture f;
    const auto photo
        = RecordConfig(Document({ { "/dActions/dPhoto/bEnabled", true }, { "/dActions/dPhoto/sFormat", "RAW" },
                           { "/dActions/dPhoto/iIntervalMs", 1500 } }),
            f.ctx)
              .createWriterSettings(f.ctx);
    VG_REQUIRE(photo != nullptr);
    VG_CHECK(photo->type == CaptureType::Image);
    VG_CHECK_EQ(photo->format, "RAW");
    VG_CHECK_EQ(photo->interval.count(), 1500L);
    VG_CHECK_EQ(photo->file_name_prefix, kRecordWriterPrefixDefault);
    VG_CHECK_EQ(photo->video_clip_length.count(), kVideoClipLengthDefault.count());
    VG_CHECK(photo->watermarks == WaterMarks::from("RAW", 1, nullptr));

    const auto video = RecordConfig(
        Document({ { "/dActions/dVideo/bEnabled", true }, { "/dActions/dVideo/iIntervalMs", 2500 } }), f.ctx)
                           .createWriterSettings(f.ctx);
    VG_REQUIRE(video != nullptr);
    VG_CHECK(video->type == CaptureType::Video);
    VG_CHECK_EQ(video->format, "MP4");
    VG_CHECK_EQ(video->interval.count(), 2500L);
    VG_CHECK(video->watermarks == WaterMarks::from("MP4", kVideoClipLengthDefault.count(), nullptr));
}

VG_TEST_CASE(sync_publishes_the_settings_and_the_rule_flag)
{
    Fixture f;
    const auto rev0 = f.ctx.shared_writer_settings.getRevision();
    const RecordConfig photo(Document({ { "/bRuleEnabled", true }, { "/dActions/dPhoto/bEnabled", true } }), f.ctx);
    VG_CHECK_EQ(photo.sync(f.ctx), 0);
    VG_CHECK(f.ctx.record_rule_enabled.load());
    VG_CHECK_EQ(f.ctx.shared_writer_settings.get().second->format, "JPG");
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 1);

    // No enabled file action: the flag is published and the writer settings stay as they were.
    VG_CHECK_EQ(RecordConfig(Document(), f.ctx).sync(f.ctx), 0);
    VG_CHECK(!f.ctx.record_rule_enabled.load());
    VG_CHECK_EQ(f.ctx.shared_writer_settings.get().second->format, "JPG");
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 1);
}

VG_TEST_CASE(default_writer_settings_exist_before_any_configuration)
{
    Fixture f;
    const auto settings = f.ctx.shared_writer_settings.get().second;
    VG_REQUIRE(settings != nullptr);
    VG_CHECK(settings->type == CaptureType::Video);
    VG_CHECK_EQ(settings->format, kRecordWriterDefaultFormat);
    VG_CHECK_EQ(settings->interval.count(), kRecordWriterDefaultIntervalMs.count());
    VG_CHECK_EQ(settings->video_clip_length.count(), kVideoClipLengthDefault.count());
    VG_CHECK_EQ(settings->file_name_prefix, kRecordWriterPrefixDefault);
    VG_CHECK(!f.ctx.record_rule_enabled.load());
}

VG_TEST_CASE(watermarks_follow_the_format_table_unless_the_host_estimates)
{
    constexpr FileSize kMiB = 1024 * 1024;
    constexpr FileSize kMinFree = static_cast<FileSize>(kStorageMinFileSystemFreeBytes);

    const auto jpg = WaterMarks::from("JPG", 1, nullptr); // 10 MiB per image
    VG_CHECK_EQ(jpg.critical_remain_bytes, 2 * 10 * kMiB + kMinFree);
    VG_CHECK_EQ(jpg.high_remain_bytes, 5 * 10 * kMiB + kMinFree);
    VG_CHECK_EQ(jpg.recycle_bytes, 10 * 10 * kMiB + kMinFree);
    VG_CHECK(WaterMarks::from("JPG", 0, nullptr) == jpg); // batch is at least one

    const auto mp4 = WaterMarks::from("MP4", 60, nullptr); // 3 MiB per second, one 60-second clip
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
    VG_CHECK_EQ(estimated.critical_remain_bytes, 2 * kMiB + kMinFree);
    media.max_bytes_estimate_cb = &EstimateNothing; // a non-positive estimate keeps the table value
    VG_CHECK(WaterMarks::from("MP4", 60, &media) == mp4);
}

VG_TEST_MAIN()
