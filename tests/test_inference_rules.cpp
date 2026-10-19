// Characterization tests for InferenceRule and InferenceRuleSet (docs/PLAN.md section 5.2, matrix R1).
// They pin today's behavior: the saturating debounce counter, the source/confidence/class/ROI filters,
// debounce zero, and first-match selection. Only input that reaches the rule moves the counter; the
// rule never sees direct EVENT input because the rule loop handles that tick itself.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

using Detections = std::vector<std::pair<std::string, float>>;

nlohmann::json RuleJson(const std::string &id, int debounce, const std::vector<std::string> &classes,
    const std::vector<std::string> &sources = {}, float conf_lb = 0.5f, float conf_ub = 1.0f)
{
    nlohmann::json j;
    j["sID"] = id;
    j["iDebounceTimes"] = debounce;
    j["lConfidenceFilter"] = { conf_lb, conf_ub };
    j["lClassFilter"] = classes;
    if (!sources.empty())
        j["lSourceFilter"] = sources;
    return j;
}

// A detection frame. Every detection sits at the same small bounding box unless a box is given.
InferenceResult Frame(int64_t ts, const std::string &source, const Detections &detections,
    BoundingBox<uint8_t> box = { { 10, 10 }, { 20, 20 } })
{
    InferenceResult r;
    r.timestamp_ms = ts;
    r.source_id = source;
    DetectionResultVec vec;
    for (const auto &[cls, conf]: detections)
        vec.push_back(DetectionResult { .class_name = cls, .confidence = conf, .object = box });
    r.data = std::move(vec);
    return r;
}

InferenceResult ClassificationFrame(int64_t ts, const std::string &source, const Detections &classes)
{
    InferenceResult r;
    r.timestamp_ms = ts;
    r.source_id = source;
    ClassificationResultVec vec;
    for (const auto &[cls, conf]: classes)
        vec.push_back(ClassificationResult { .class_name = cls, .confidence = conf });
    r.data = std::move(vec);
    return r;
}

const Detections kPerson = { { "person", 0.9f } };
const Detections kNothing = {};

} // namespace

VG_TEST_CASE(inference_rule_counts_subscribers_for_its_lifetime)
{
    Fixture f;
    const InferenceRuleConfig cfg(RuleJson("person", 1, { "person" }), f.ctx);
    VG_CHECK_EQ(f.ctx.inference_subscribers.load(), 0);
    {
        const InferenceRule rule(cfg, f.ctx);
        VG_CHECK_EQ(f.ctx.inference_subscribers.load(), 1);
        VG_CHECK_EQ(rule.type(), VG_RULE_TYPE_INFERENCE);
        VG_CHECK_EQ(rule.id(), "person");
    }
    VG_CHECK_EQ(f.ctx.inference_subscribers.load(), 0);
}

VG_TEST_CASE(debounce_counter_saturates_and_decays_one_step_per_negative_frame)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 3, { "person" }), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1000, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr); // counter 1
    f.ctx.shared_inference_result = Frame(1001, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr); // counter 2
    f.ctx.shared_inference_result = Frame(1002, "cam0", kPerson);
    const auto owner = rule(f.ctx); // counter 3: active
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->type, VG_RULE_TYPE_INFERENCE);
    VG_CHECK_EQ(owner->id, "person");
    VG_CHECK_EQ(owner->source_id, "cam0");
    VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Frame);
    VG_CHECK_EQ(owner->timestamp, int64_t { 1002 }); // the input event time, not the wall clock

    // The counter saturates at the threshold ...
    f.ctx.shared_inference_result = Frame(1003, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr);
    // ... so one negative frame always drops activation, however long the input was positive.
    f.ctx.shared_inference_result = Frame(1004, "cam0", kNothing);
    VG_CHECK(rule(f.ctx) == nullptr); // counter 2
    // One positive frame restores it (2 -> 3).
    f.ctx.shared_inference_result = Frame(1005, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr);

    // Negative frames decay one step each and stop at zero.
    for (int64_t i = 0; i < 5; ++i)
    {
        f.ctx.shared_inference_result = Frame(1010 + i, "cam0", kNothing);
        VG_CHECK(rule(f.ctx) == nullptr);
    }
    // From zero, exactly three positive frames are needed again.
    f.ctx.shared_inference_result = Frame(1020, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(1021, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(1022, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr);
}

VG_TEST_CASE(missing_input_does_not_change_the_counter)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 3, { "person" }), f.ctx), f.ctx);

    for (int64_t i = 0; i < 3; ++i)
        f.ctx.shared_inference_result = Frame(100 + i, "cam0", kPerson), (void)rule(f.ctx); // counter 3

    f.ctx.shared_inference_result.reset(); // no input on this tick
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(200, "cam0", kNothing);
    VG_CHECK(rule(f.ctx) == nullptr); // counter 2
    // If the empty tick had also decremented, this positive frame would only reach 2 and stay inactive.
    f.ctx.shared_inference_result = Frame(201, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr);
}

VG_TEST_CASE(unrelated_source_is_filtered_before_the_counter)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 2, { "person" }, { "cam0" }), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr); // counter 1
    f.ctx.shared_inference_result = Frame(2, "cam1", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr); // filtered, counter still 1
    f.ctx.shared_inference_result = Frame(3, "cam1", kNothing);
    VG_CHECK(rule(f.ctx) == nullptr); // filtered, counter still 1
    f.ctx.shared_inference_result = Frame(4, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr); // counter 2: active
}

VG_TEST_CASE(without_a_source_filter_any_source_counts)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 2, { "person" }), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam7", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(2, "cam9", kPerson);
    const auto owner = rule(f.ctx);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->source_id, "cam9"); // the source of the activating frame
}

VG_TEST_CASE(debounce_zero_activates_on_every_frame_including_negatives)
{
    // Pinned as implemented on 2026-10-19 (docs/PLAN.md section 2): with iDebounceTimes = 0 every frame
    // from an admitted source activates, whether or not it contains a matching detection.
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 0, { "person" }, { "cam0" }), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam0", kNothing);
    VG_CHECK(rule(f.ctx) != nullptr);
    f.ctx.shared_inference_result = Frame(2, "cam0", { { "cat", 0.9f } });
    VG_CHECK(rule(f.ctx) != nullptr);
    f.ctx.shared_inference_result = Frame(3, "cam0", kPerson);
    VG_CHECK(rule(f.ctx) != nullptr);
    // The source filter still applies before the counter.
    f.ctx.shared_inference_result = Frame(4, "cam1", kPerson);
    VG_CHECK(rule(f.ctx) == nullptr);
    // And an empty tick still produces nothing.
    f.ctx.shared_inference_result.reset();
    VG_CHECK(rule(f.ctx) == nullptr);
}

VG_TEST_CASE(confidence_and_class_filters_select_matching_detections)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 1, { "person" }, {}, 0.5f, 0.8f), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam0", { { "person", 0.49f } });
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(2, "cam0", { { "person", 0.81f } });
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = Frame(3, "cam0", { { "cat", 0.7f } });
    VG_CHECK(rule(f.ctx) == nullptr);
    // One matching detection among others is enough; both range ends are inclusive.
    f.ctx.shared_inference_result = Frame(4, "cam0", { { "cat", 0.7f }, { "person", 0.5f } });
    VG_CHECK(rule(f.ctx) != nullptr);
    f.ctx.shared_inference_result = Frame(5, "cam0", { { "person", 0.8f } });
    VG_CHECK(rule(f.ctx) != nullptr);
}

VG_TEST_CASE(empty_inference_class_filter_matches_any_class)
{
    // Unlike the SED rule, an empty class filter on an inference rule accepts every class.
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("any", 1, {}), f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam0", { { "whatever", 0.9f } });
    VG_CHECK(rule(f.ctx) != nullptr);
    f.ctx.shared_inference_result = Frame(2, "cam0", kNothing);
    VG_CHECK(rule(f.ctx) == nullptr);
}

VG_TEST_CASE(classification_results_use_the_same_filters)
{
    Fixture f;
    const InferenceRule rule(InferenceRuleConfig(RuleJson("person", 1, { "person" }), f.ctx), f.ctx);

    f.ctx.shared_inference_result = ClassificationFrame(1, "cam0", { { "cat", 0.9f } });
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = ClassificationFrame(2, "cam0", { { "person", 0.4f } });
    VG_CHECK(rule(f.ctx) == nullptr);
    f.ctx.shared_inference_result = ClassificationFrame(3, "cam0", { { "person", 0.9f } });
    VG_CHECK(rule(f.ctx) != nullptr);
}

VG_TEST_CASE(region_filter_tests_the_detection_center)
{
    Fixture f;
    auto j = RuleJson("person", 1, { "person" });
    // Left half of the normalized frame.
    j["lRegionFilter"] = { { { "lPolygon", { { 0.0f, 0.0f }, { 0.5f, 0.0f }, { 0.5f, 1.0f }, { 0.0f, 1.0f } } } } };
    const InferenceRule rule(InferenceRuleConfig(j, f.ctx), f.ctx);

    f.ctx.shared_inference_result = Frame(1, "cam0", kPerson, { { 10, 10 }, { 20, 20 } }); // center (15, 15)
    VG_CHECK(rule(f.ctx) != nullptr);
    f.ctx.shared_inference_result = Frame(2, "cam0", kPerson, { { 100, 10 }, { 110, 20 } }); // center (105, 15)
    VG_CHECK(rule(f.ctx) == nullptr);
}

VG_TEST_CASE(rule_config_rejects_malformed_filters)
{
    Fixture f;
    auto bad_source = RuleJson("r", 1, { "person" }, { "" });
    VG_CHECK_THROWS(InferenceRuleConfig(bad_source, f.ctx));
    auto long_source = RuleJson("r", 1, { "person" }, { std::string(129, 'x') });
    VG_CHECK_THROWS(InferenceRuleConfig(long_source, f.ctx));
    auto inverted_confidence = RuleJson("r", 1, { "person" }, {}, 0.9f, 0.1f);
    VG_CHECK_THROWS(InferenceRuleConfig(inverted_confidence, f.ctx));
    auto out_of_range_confidence = RuleJson("r", 1, { "person" }, {}, 0.0f, 1.5f);
    VG_CHECK_THROWS(InferenceRuleConfig(out_of_range_confidence, f.ctx));
    auto class_not_array = RuleJson("r", 1, { "person" });
    class_not_array["lClassFilter"] = "person";
    VG_CHECK_THROWS(InferenceRuleConfig(class_not_array, f.ctx));
    auto degenerate_polygon = RuleJson("r", 1, { "person" });
    degenerate_polygon["lRegionFilter"] = { { { "lPolygon", { { 0.0f, 0.0f }, { 1.0f, 1.0f } } } } };
    VG_CHECK_THROWS(InferenceRuleConfig(degenerate_polygon, f.ctx));
    auto missing_id = RuleJson("r", 1, { "person" });
    missing_id.erase("sID");
    VG_CHECK_THROWS(InferenceRuleConfig(missing_id, f.ctx));
}

VG_TEST_CASE(rule_set_returns_the_first_match_and_skips_later_members_on_that_tick)
{
    Fixture f;
    const nlohmann::json set_j = { RuleJson("a", 1, { "cat" }), RuleJson("b", 2, { "dog" }) };
    const InferenceRuleSet set(InferenceRuleSetConfig(set_j, f.ctx), f.ctx);
    VG_CHECK_EQ(f.ctx.inference_subscribers.load(), 2);
    VG_CHECK_EQ(set.type(), VG_RULE_TYPE_INFERENCE_SET);

    f.ctx.shared_inference_result = Frame(1, "cam0", { { "cat", 0.9f }, { "dog", 0.9f } });
    auto owner = set(f.ctx);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->type, VG_RULE_TYPE_INFERENCE_SET); // the set's type, the member's id
    VG_CHECK_EQ(owner->id, "a");
    VG_CHECK_EQ(set.id(), "a");

    // "b" was not evaluated on that tick: it still needs two dog frames.
    f.ctx.shared_inference_result = Frame(2, "cam0", { { "dog", 0.9f } });
    VG_CHECK(set(f.ctx) == nullptr);
    VG_CHECK_EQ(set.id(), ""); // cleared on every evaluation
    f.ctx.shared_inference_result = Frame(3, "cam0", { { "dog", 0.9f } });
    owner = set(f.ctx);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->id, "b");
    VG_CHECK_EQ(set.id(), "b");

    f.ctx.shared_inference_result = Frame(4, "cam0", kNothing);
    VG_CHECK(set(f.ctx) == nullptr);
}

VG_TEST_MAIN()
