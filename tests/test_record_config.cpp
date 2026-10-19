// Characterization tests for the record action configuration and the shared writer settings
// (docs/PLAN.md sections 2 and 4.3; matrix C1/C2). The fixtures use today's shape (bRuleEnabled +
// dWriterConfig); Phase 1C replaces them with the version-2 shape and keeps the assertions on
// validation and revision handling.

#include <cerrno>
#include <cstddef>
#include <cstdint>

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

nlohmann::json RecordJson(bool enabled, const char *format, long interval_ms)
{
    return nlohmann::json {
        { "bRuleEnabled", enabled },
        { "dWriterConfig", { { "sFormat", format }, { "iIntervalMs", interval_ms } } },
    };
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

VG_TEST_CASE(default_settings_exist_before_any_configuration)
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

    nlohmann::json j;
    RecordConfig::from(f.ctx).to(j, f.ctx);
    VG_CHECK_EQ(j, RecordJson(false, "MP4", 5000));
}

VG_TEST_CASE(parses_the_current_shape_and_round_trips)
{
    Fixture f;
    const auto j = RecordJson(true, "JPG", 1500);
    const RecordConfig cfg(j, f.ctx);
    VG_CHECK(cfg.enabled);
    VG_REQUIRE(cfg.settings != nullptr);
    VG_CHECK(cfg.settings->type == CaptureType::Image);
    VG_CHECK_EQ(cfg.settings->format, "JPG");
    VG_CHECK_EQ(cfg.settings->interval.count(), 1500L);
    VG_CHECK_EQ(cfg.settings->file_name_prefix, kRecordWriterPrefixDefault);
    VG_CHECK_EQ(cfg.settings->video_clip_length.count(), kVideoClipLengthDefault.count());
    VG_CHECK_EQ(cfg.settings_rev, f.ctx.shared_writer_settings.getRevision()); // prepared against the current revision

    nlohmann::json out;
    cfg.to(out, f.ctx);
    VG_CHECK_EQ(out, j);

    const RecordConfig raw(RecordJson(false, "RAW", 100), f.ctx);
    VG_CHECK(!raw.enabled);
    VG_CHECK(raw.settings->type == CaptureType::Image);
    VG_CHECK_EQ(raw.settings->format, "RAW");

    // Unknown fields are ignored.
    auto extra = j;
    extra["sUnknown"] = 1;
    extra["dWriterConfig"]["iExtra"] = 2;
    const RecordConfig with_extra(extra, f.ctx);
    VG_CHECK(with_extra.enabled);
}

VG_TEST_CASE(rejects_non_positive_intervals_unknown_formats_and_wrong_types)
{
    Fixture f;
    // Pinned on 2026-10-19 (docs/PLAN.md section 2): a zero interval is rejected, not treated as "no gap".
    VG_CHECK_THROWS(RecordConfig(RecordJson(true, "MP4", 0), f.ctx));
    VG_CHECK_THROWS(RecordConfig(RecordJson(true, "MP4", -1), f.ctx));
    VG_CHECK_THROWS(RecordConfig(RecordJson(true, "AVI", 1000), f.ctx));
    VG_CHECK_THROWS(RecordConfig(RecordJson(true, "mp4", 1000), f.ctx)); // case-sensitive

    auto wrong_enabled = RecordJson(true, "MP4", 1000);
    wrong_enabled["bRuleEnabled"] = "yes";
    VG_CHECK_THROWS(RecordConfig(wrong_enabled, f.ctx));
    auto missing_enabled = RecordJson(true, "MP4", 1000);
    missing_enabled.erase("bRuleEnabled");
    VG_CHECK_THROWS(RecordConfig(missing_enabled, f.ctx));
    auto missing_writer = RecordJson(true, "MP4", 1000);
    missing_writer.erase("dWriterConfig");
    VG_CHECK_THROWS(RecordConfig(missing_writer, f.ctx));
    auto string_interval = RecordJson(true, "MP4", 1000);
    string_interval["dWriterConfig"]["iIntervalMs"] = "1000";
    VG_CHECK_THROWS(RecordConfig(string_interval, f.ctx));
    auto missing_format = RecordJson(true, "MP4", 1000);
    missing_format["dWriterConfig"].erase("sFormat");
    VG_CHECK_THROWS(RecordConfig(missing_format, f.ctx));
}

VG_TEST_CASE(sync_applies_the_snapshot_only_at_the_expected_revision)
{
    Fixture f;
    const auto rev0 = f.ctx.shared_writer_settings.getRevision();

    const RecordConfig first(RecordJson(true, "MP4", 2000), f.ctx);
    VG_CHECK_EQ(first.sync(f.ctx), 0);
    VG_CHECK(f.ctx.record_rule_enabled.load());
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 1);
    VG_CHECK_EQ(f.ctx.shared_writer_settings.get().second->interval.count(), 2000L);
    VG_CHECK_EQ(RecordConfig::from(f.ctx).settings->interval.count(), 2000L);

    // Two configurations prepared against the same revision: the second one to commit loses.
    const RecordConfig second(RecordJson(true, "MP4", 2500), f.ctx);
    const RecordConfig third(RecordJson(true, "JPG", 3000), f.ctx);
    VG_CHECK_EQ(second.sync(f.ctx), 0);
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 2);
    VG_CHECK_EQ(third.sync(f.ctx), EAGAIN);
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 2);
    VG_CHECK_EQ(f.ctx.shared_writer_settings.get().second->format, "MP4");
    VG_CHECK_EQ(f.ctx.shared_writer_settings.get().second->interval.count(), 2500L);
    // Note: today sync() stores bRuleEnabled before the revision check, so a losing commit still changes
    // enablement. Phase 1C makes a conflict leave both unchanged (docs/PLAN.md section 4.3) and adds that
    // assertion here.

    // Re-committing identical settings succeeds without bumping the revision.
    const RecordConfig same(RecordJson(true, "MP4", 2500), f.ctx);
    VG_CHECK_EQ(same.sync(f.ctx), 0);
    VG_CHECK_EQ(f.ctx.shared_writer_settings.getRevision(), rev0 + 2);
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

    // Not asserted: WaterMarks::from with an unknown format reads past the end of its format table instead
    // of reaching the fallback entry (found 2026-10-19). It is unreachable today because every caller
    // validates the format first; the fix is tracked in docs/TODO.md (SAFE-7).

    vg_config_media_t media {};
    media.max_bytes_estimate_cb = &EstimateOneMiB;
    const auto estimated = WaterMarks::from("MP4", 60, &media);
    VG_CHECK_EQ(estimated.critical_remain_bytes, 2 * kMiB + kMinFree);
    media.max_bytes_estimate_cb = &EstimateNothing; // a non-positive estimate keeps the table value
    VG_CHECK(WaterMarks::from("MP4", 60, &media) == mp4);
}

VG_TEST_MAIN()
