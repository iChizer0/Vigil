// ScheduleRuleConfig and ScheduleRule: a disabled schedule is open, an enabled one inside any configured weekly
// window, and windows may wrap around the end of the week.

#include <cstddef>

#include "vigil/configs.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/rules.hpp"
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::rules;
using namespace vigil::tests;

namespace {

struct Fixture
{
    Context ctx;

    Fixture() : ctx(MakeFakeConfig()) { }
};

constexpr int kSun = 0;
constexpr int kMon = 1;
constexpr int kTue = 2;
constexpr int kWed = 3;
constexpr int kFri = 5;
constexpr int kSat = 6;

nlohmann::json ScheduleJson(bool enabled, std::initializer_list<std::pair<const char *, const char *>> windows)
{
    nlohmann::json j;
    j["bEnabled"] = enabled;
    j["lActiveWeekdays"] = nlohmann::json::array();
    for (const auto &[start, end]: windows)
        j["lActiveWeekdays"].push_back({ { "sStart", start }, { "sEnd", end } });
    return j;
}

} // namespace

VG_TEST_CASE(default_schedule_is_disabled_and_therefore_open)
{
    Fixture f;
    const ScheduleRuleConfig cfg;
    VG_CHECK(!cfg.enabled);
    VG_CHECK_EQ(cfg.active_weekdays.size(), size_t { 1 }); // a full week, used once the gate is enabled

    const ScheduleRule rule(cfg, f.ctx);
    VG_CHECK_EQ(rule.type(), VG_RULE_TYPE_SCHEDULE);
    VG_CHECK(rule.isActive(LocalTime(kSun, 0, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kWed, 12, 30, 0)));
    VG_CHECK(rule.isActive(LocalTime(kSat, 23, 59, 59)));
}

VG_TEST_CASE(enabled_window_is_inclusive_at_both_ends)
{
    Fixture f;
    const ScheduleRule rule(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 09:00:00", "Mon 17:00:00" } }), f.ctx),
        f.ctx);

    VG_CHECK(rule.isActive(LocalTime(kMon, 9, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kMon, 12, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kMon, 17, 0, 0)));
    VG_CHECK(!rule.isActive(LocalTime(kMon, 8, 59, 59)));
    VG_CHECK(!rule.isActive(LocalTime(kMon, 17, 0, 1)));
    VG_CHECK(!rule.isActive(LocalTime(kTue, 12, 0, 0)));
    VG_CHECK(!rule.isActive(LocalTime(kSun, 12, 0, 0)));
}

VG_TEST_CASE(window_that_crosses_the_end_of_the_week_wraps)
{
    Fixture f;
    const ScheduleRule rule(ScheduleRuleConfig(ScheduleJson(true, { { "Fri 22:00:00", "Mon 06:00:00" } }), f.ctx),
        f.ctx);

    VG_CHECK(rule.isActive(LocalTime(kFri, 23, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kSat, 12, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kSun, 0, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kMon, 5, 59, 59)));
    VG_CHECK(!rule.isActive(LocalTime(kMon, 6, 0, 1)));
    VG_CHECK(!rule.isActive(LocalTime(kWed, 12, 0, 0)));
    VG_CHECK(!rule.isActive(LocalTime(kFri, 21, 59, 59)));
}

VG_TEST_CASE(disabled_schedule_ignores_its_windows)
{
    Fixture f;
    const ScheduleRule rule(ScheduleRuleConfig(ScheduleJson(false, { { "Mon 09:00:00", "Mon 17:00:00" } }), f.ctx),
        f.ctx);
    VG_CHECK(rule.isActive(LocalTime(kTue, 12, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kMon, 3, 0, 0)));
}

VG_TEST_CASE(several_windows_form_a_union)
{
    Fixture f;
    const ScheduleRule rule(
        ScheduleRuleConfig(
            ScheduleJson(true, { { "Mon 09:00:00", "Mon 12:00:00" }, { "Wed 00:00:00", "Wed 24:00:00" } }), f.ctx),
        f.ctx);
    VG_CHECK(rule.isActive(LocalTime(kMon, 10, 0, 0)));
    VG_CHECK(rule.isActive(LocalTime(kWed, 23, 59, 59)));
    VG_CHECK(!rule.isActive(LocalTime(kMon, 13, 0, 0)));
    VG_CHECK(!rule.isActive(LocalTime(kTue, 10, 0, 0)));
}

VG_TEST_CASE(parser_rejects_malformed_windows)
{
    Fixture f;
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 09:00:00", "Mon 09:00:00" } }), f.ctx));
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Xyz 09:00:00", "Mon 10:00:00" } }), f.ctx));
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 09:00:00", "Mon 24:30:00" } }), f.ctx));
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 25:00:00", "Mon 26:00:00" } }), f.ctx));
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 09:60:00", "Mon 10:00:00" } }), f.ctx));
    VG_CHECK_THROWS(ScheduleRuleConfig(ScheduleJson(true, { { "Mon 09:00", "Mon 10:00:00" } }), f.ctx));
    auto missing_enabled = ScheduleJson(true, { { "Mon 09:00:00", "Mon 10:00:00" } });
    missing_enabled.erase("bEnabled");
    VG_CHECK_THROWS(ScheduleRuleConfig(missing_enabled, f.ctx));
    auto missing_windows = ScheduleJson(true, {});
    missing_windows.erase("lActiveWeekdays");
    VG_CHECK_THROWS(ScheduleRuleConfig(missing_windows, f.ctx));

    // 24:00:00 is the end-of-day sentinel and is accepted.
    const ScheduleRuleConfig end_of_day(ScheduleJson(true, { { "Mon 00:00:00", "Mon 24:00:00" } }), f.ctx);
    VG_CHECK_EQ(end_of_day.active_weekdays.size(), size_t { 1 });
    // An enabled schedule without windows parses and is closed everywhere.
    const ScheduleRule closed(ScheduleRuleConfig(ScheduleJson(true, {}), f.ctx), f.ctx);
    VG_CHECK(!closed.isActive(LocalTime(kMon, 12, 0, 0)));
}

VG_TEST_CASE(serialization_round_trips)
{
    Fixture f;
    const auto j = ScheduleJson(true, { { "Mon 09:00:00", "Mon 17:00:00" }, { "Fri 22:00:00", "Mon 06:00:00" } });
    const ScheduleRuleConfig cfg(j, f.ctx);

    nlohmann::json out;
    cfg.to(out, f.ctx);
    VG_CHECK_EQ(out, j);

    const ScheduleRuleConfig again(out, f.ctx);
    VG_CHECK(again == cfg);

    const ScheduleRuleConfig different(ScheduleJson(true, { { "Mon 09:00:00", "Mon 17:00:01" } }), f.ctx);
    VG_CHECK(!(different == cfg));
}

VG_TEST_MAIN()
