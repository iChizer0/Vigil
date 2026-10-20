// Characterization tests for the GPIO, TTY, and SED rules over fake peripherals (docs/PLAN.md sections 5.2 and
// 6.1; matrix R2): the sync callback the selected GPIO signal mode needs, level and edge evaluation, the TTY
// command matcher (overlapping prefixes, commands split across reads, several commands in one read, reads
// larger than the buffer), the SED rule as observed (its filters, the consecutive window, the latched
// activation, ignored and malformed frames, the reconnect backoff, and a prompt stop), and the owner
// attribution the rules carry. The TTY and SED peers are Unix sockets in a temporary directory.

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "envelope.pb.h"

#include "vigil/configs.hpp"
#include "vigil/context.hpp"
#include "vigil/json.hpp" // IWYU pragma: keep
#include "vigil/rules.hpp"
#include "vigil/types.hpp"

#include "check.hpp"
#include "fake_config.hpp"

namespace vigil::rules {

// Reaches the two pure parts of the SED rule's poll loop, so that the frame filter and the consecutive window
// are pinned with explicit inputs and times.
struct SoundEventRuleTestAccess
{
    using DecodeStatus = SoundEventRule::DecodeStatus;

    static DecodeStatus filter(const SoundEventRule &rule, const std::string &frame)
    {
        return rule.filterEnvelope(reinterpret_cast<const uint8_t *>(frame.data()), frame.size());
    }

    static bool finalActive(const SoundEventRule &rule, bool active,
        std::optional<std::chrono::steady_clock::time_point> &active_since, std::chrono::steady_clock::time_point now)
    {
        return rule.finalActive(active, active_since, now);
    }
};

} // namespace vigil::rules

using namespace vigil;
using namespace vigil::types;
using namespace vigil::configs;
using namespace vigil::rules;
using namespace vigil::tests;

namespace {

// One fake pin. The test sets the level and latches an edge; the edge is consumed when it is read.
struct FakeGPIO
{
    static inline vg_config_gpio_pin_t pins[1] = { { .num = 7,
        .name = "GPIO_07",
        .state = VG_GPIO_PIN_MODE_INP_FLOATING,
        .capabilities = static_cast<vg_gpio_pin_mod_e>(VG_GPIO_PIN_MODE_INP_FLOATING | VG_GPIO_PIN_MODE_INP_PULLUP) } };
    static inline int acquired = 0;
    static inline int released = 0;
    static inline int last_pin = -1;
    static inline vg_gpio_pin_mod_e last_mode = VG_GPIO_PIN_MODE_DISABLED;
    static inline size_t last_debounce_ms = 0;
    static inline vg_gpio_pin_lvl_e level = VG_GPIO_PIN_LVL_LOW;
    static inline vg_gpio_pin_evt_e event = VG_GPIO_PIN_EVT_NONE;

    static void reset()
    {
        acquired = released = 0;
        last_pin = -1;
        last_mode = VG_GPIO_PIN_MODE_DISABLED;
        last_debounce_ms = 0;
        level = VG_GPIO_PIN_LVL_LOW;
        event = VG_GPIO_PIN_EVT_NONE;
    }

    static vg_sta_e Available(vg_config_gpio_pins_t *out)
    {
        out->free_cb = nullptr;
        out->pins = pins;
        out->len = 1;
        return 0;
    }

    static vg_sta_e Acquire(int pin, vg_gpio_pin_mod_e mode, size_t debounce_ms)
    {
        ++acquired;
        last_pin = pin;
        last_mode = mode;
        last_debounce_ms = debounce_ms;
        return 0;
    }

    static void Release(int pin)
    {
        ++released;
        last_pin = pin;
    }

    static vg_gpio_pin_lvl_e SyncLevel(int)
    {
        return level;
    }

    static vg_gpio_pin_evt_e SyncEvent(int)
    {
        const auto e = event;
        event = VG_GPIO_PIN_EVT_NONE;
        return e;
    }
};

vg_config_t GPIOConfig(bool with_level_cb, bool with_event_cb)
{
    auto cfg = MakeFakeConfig();
    cfg.gpios.available_cb = &FakeGPIO::Available;
    cfg.gpios.acquire_cb = &FakeGPIO::Acquire;
    cfg.gpios.release_cb = &FakeGPIO::Release;
    cfg.gpios.sync_level_cb = with_level_cb ? &FakeGPIO::SyncLevel : nullptr;
    cfg.gpios.sync_event_cb = with_event_cb ? &FakeGPIO::SyncEvent : nullptr;
    return cfg;
}

nlohmann::json GPIOJson(const char *signal)
{
    return { { "sName", "GPIO_07" }, { "sState", "PULL_UP" }, { "sSignal", signal }, { "iDebounceDurationMs", 100 } };
}

constexpr auto kWait = std::chrono::seconds(10); // the bound on every wait for the rule's own thread

// A listening Unix socket a rule connects to: the TTY peer, or the AcousticsLab result socket of the SED rule.
struct SocketPeer
{
    std::string dir;
    std::string path;
    int listen_fd = -1;
    int peer_fd = -1;

    explicit SocketPeer(const char *name = "tty.sock")
    {
        std::string p = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
        if (::mkdtemp(p.data()) == nullptr)
            throw std::runtime_error("mkdtemp failed");
        dir = p;
        path = dir + "/" + name;
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (listen_fd < 0 || ::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0
            || ::listen(listen_fd, 1) != 0)
            throw std::runtime_error("failed to listen on " + path);
    }

    ~SocketPeer()
    {
        disconnect();
        if (listen_fd >= 0)
            ::close(listen_fd);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Accepts the connection the rule made in its constructor.
    void accept()
    {
        if (!acceptWithin(kWait))
            throw std::runtime_error("accept timed out");
    }

    // Accepts the next connection, false when none arrives within `timeout`.
    bool acceptWithin(std::chrono::milliseconds timeout)
    {
        disconnect();
        ::pollfd pfd { .fd = listen_fd, .events = POLLIN, .revents = 0 };
        if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0)
            return false;
        peer_fd = ::accept(listen_fd, nullptr, nullptr);
        if (peer_fd < 0)
            throw std::runtime_error("accept failed");
        return true;
    }

    // Closes the accepted connection, if any: what the rule's peer going away looks like.
    void disconnect()
    {
        if (peer_fd >= 0)
            ::close(peer_fd);
        peer_fd = -1;
    }

    // True once the rule has closed the accepted connection (the rule never writes, so the first readable
    // event is its end of file); the connection is released then.
    bool waitForClose(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            ::pollfd pfd { .fd = peer_fd, .events = POLLIN, .revents = 0 };
            if (::poll(&pfd, 1, 100) <= 0)
                continue;
            char byte;
            const auto n = ::recv(peer_fd, &byte, 1, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            {
                disconnect();
                return true;
            }
        }
        return false;
    }

    // Bytes sent on a Unix stream socket are readable by the peer when this returns.
    void send(const std::string &bytes)
    {
        if (::send(peer_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(bytes.size()))
            throw std::runtime_error("send failed");
    }

    // One SED frame: the little-endian 32-bit payload length, then the payload.
    void sendFrame(const std::string &payload)
    {
        send(Prefix(static_cast<uint32_t>(payload.size())) + payload);
    }

    static std::string Prefix(uint32_t length)
    {
        std::string p(4, '\0');
        for (size_t i = 0; i < 4; ++i)
            p[i] = static_cast<char>((length >> (8 * i)) & 0xff);
        return p;
    }
};

struct TTYFixture
{
    SocketPeer peer;
    vg_config_tty_t tty; // borrows peer.path
    Context ctx;

    explicit TTYFixture(size_t buffer_size = 16)
        : peer(), tty { .name = "tty0", .socket_path = peer.path.c_str(), .buffer_size = buffer_size }, ctx(Config(tty))
    {
    }

    static vg_config_t Config(vg_config_tty_t &tty)
    {
        auto cfg = MakeFakeConfig();
        cfg.ttys.ttys = &tty;
        cfg.ttys.len = 1;
        return cfg;
    }

    std::unique_ptr<TTYRule> rule(const char *command)
    {
        auto r = std::make_unique<TTYRule>(
            TTYRuleConfig(nlohmann::json { { "sName", "tty0" }, { "sCommand", command } }, ctx), ctx);
        peer.accept();
        return r;
    }
};

// Spins until `pred` holds, false after kWait: a bounded wait on the rule's thread, not a timing assumption.
template<typename Pred>
bool WaitFor(Pred pred)
{
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

nlohmann::json SEDJson(const char *id, int window_ms, const std::vector<std::string> &classes, float conf_lb = 0.5f,
    float conf_ub = 1.0f)
{
    nlohmann::json j;
    j["sID"] = id;
    j["iConsecutiveWindowMs"] = window_ms;
    j["lConfidenceFilter"] = { conf_lb, conf_ub };
    j["lClassFilter"] = classes;
    return j;
}

// A serialized AcousticsLab envelope carrying one inference frame with one top-k entry.
std::string InferenceEnvelope(std::optional<std::string> head_id, const std::string &label, float prob)
{
    acousticslab::v1::Envelope envelope;
    auto *inference = envelope.mutable_inference();
    if (head_id)
        inference->set_head_id(*head_id);
    auto *top = inference->add_top_k();
    top->set_label(label);
    top->set_prob(prob);
    return envelope.SerializeAsString();
}

// A serialized envelope carrying an audio frame: input the rule ignores.
std::string AudioEnvelope()
{
    acousticslab::v1::Envelope envelope;
    envelope.mutable_audio();
    return envelope.SerializeAsString();
}

const std::string kMalformedEnvelope = std::string("\x5a\x05\x01", 3); // a length-delimited field cut short

struct SEDFixture
{
    SocketPeer peer;
    Context ctx;

    SEDFixture() : peer("acousticslab.sock"), ctx(MakeFakeConfig()) { }

    // A rule connected to the peer's socket (the production rule reads kRuleSoundEventUDSPath instead).
    std::unique_ptr<SoundEventRule> rule(const nlohmann::json &j)
    {
        return std::make_unique<SoundEventRule>(SoundEventRuleConfig(j, ctx), ctx, peer.path);
    }

    // A rule whose socket nobody listens on.
    std::unique_ptr<SoundEventRule> unconnectedRule(const nlohmann::json &j)
    {
        return std::make_unique<SoundEventRule>(SoundEventRuleConfig(j, ctx), ctx, peer.dir + "/nobody.sock");
    }

    // The owner of the next activation, null when none is latched within kWait.
    std::shared_ptr<EventOwner> latched(const SoundEventRule &rule)
    {
        std::shared_ptr<EventOwner> owner;
        WaitFor([&] {
            owner = rule(ctx);
            return owner != nullptr;
        });
        return owner;
    }
};

using Clock = std::chrono::steady_clock;
using Status = SoundEventRuleTestAccess::DecodeStatus;

} // namespace

VG_TEST_CASE(gpio_rule_requires_the_sync_callback_of_its_signal_mode)
{
    FakeGPIO::reset();
    {
        const Context ctx(GPIOConfig(/* with_level_cb = */ true, /* with_event_cb = */ false));
        VG_CHECK_THROWS(GPIORule(GPIORuleConfig(GPIOJson("RISING"), ctx), ctx)); // an edge needs sync_event_cb
        VG_CHECK_EQ(FakeGPIO::acquired, 0);                                      // rejected before acquiring
        const GPIORule level(GPIORuleConfig(GPIOJson("HIGH"), ctx), ctx);
        VG_CHECK_EQ(FakeGPIO::acquired, 1);
        VG_CHECK_EQ(FakeGPIO::last_pin, 7);
        VG_CHECK(FakeGPIO::last_mode == VG_GPIO_PIN_MODE_INP_PULLUP);
        VG_CHECK_EQ(FakeGPIO::last_debounce_ms, size_t { 100 });
    }
    VG_CHECK_EQ(FakeGPIO::released, 1); // destruction releases the pin
    {
        const Context ctx(GPIOConfig(/* with_level_cb = */ false, /* with_event_cb = */ true));
        VG_CHECK_THROWS(GPIORule(GPIORuleConfig(GPIOJson("LOW"), ctx), ctx)); // a level needs sync_level_cb
        const GPIORule edge(GPIORuleConfig(GPIOJson("FALLING"), ctx), ctx);
        VG_CHECK_EQ(FakeGPIO::acquired, 2);
    }
    {
        const Context ctx(GPIOConfig(false, false));
        VG_CHECK_THROWS(GPIORule(GPIORuleConfig(GPIOJson("HIGH"), ctx), ctx));
    }
    VG_CHECK_EQ(FakeGPIO::released, 2);
}

VG_TEST_CASE(gpio_rule_observes_levels_and_edges_and_names_the_signal_and_the_pin)
{
    FakeGPIO::reset();
    const Context ctx(GPIOConfig(true, true));
    {
        const GPIORule rule(GPIORuleConfig(GPIOJson("HIGH"), ctx), ctx);
        FakeGPIO::level = VG_GPIO_PIN_LVL_LOW;
        VG_CHECK(rule(ctx) == nullptr);
        FakeGPIO::level = VG_GPIO_PIN_LVL_HIGH;
        const auto owner = rule(ctx);
        VG_REQUIRE(owner != nullptr);
        VG_CHECK_EQ(owner->type, VG_RULE_TYPE_GPIO);
        VG_CHECK_EQ(owner->id, "HIGH");           // empty before 2026-10-19
        VG_CHECK_EQ(owner->source_id, "GPIO_07"); // empty before 2026-10-19
        VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Event);
        VG_CHECK(rule(ctx) != nullptr); // a sustained level retriggers on every evaluation
    }
    {
        const GPIORule rule(GPIORuleConfig(GPIOJson("RISING"), ctx), ctx);
        FakeGPIO::event = VG_GPIO_PIN_EVT_FALLING;
        VG_CHECK(rule(ctx) == nullptr); // the other edge is observed and dropped
        FakeGPIO::event = VG_GPIO_PIN_EVT_RISING;
        const auto owner = rule(ctx);
        VG_REQUIRE(owner != nullptr);
        VG_CHECK_EQ(owner->id, "RISING");
        VG_CHECK_EQ(owner->source_id, "GPIO_07");
        VG_CHECK(rule(ctx) == nullptr); // edges are observed on evaluation, not queued
    }
}

VG_TEST_CASE(tty_rule_finds_overlapping_prefixes_and_commands_split_across_reads)
{
    TTYFixture f;
    const auto rule = f.rule("AAB");
    f.peer.send("AAAB"); // the command after a longer run of its own prefix: missed before 2026-10-19
    auto owner = (*rule)(f.ctx);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->type, VG_RULE_TYPE_TTY);
    VG_CHECK_EQ(owner->id, "AAB");         // empty before 2026-10-19
    VG_CHECK_EQ(owner->source_id, "tty0"); // empty before 2026-10-19
    VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Event);
    VG_CHECK((*rule)(f.ctx) == nullptr);

    f.peer.send("xxAA"); // a command split across two reads
    VG_CHECK((*rule)(f.ctx) == nullptr);
    f.peer.send("Bxx");
    VG_CHECK((*rule)(f.ctx) != nullptr);

    f.peer.send("ABAB"); // never completes the command
    VG_CHECK((*rule)(f.ctx) == nullptr);

    f.peer.send("AABAAB"); // two commands in one read: one activation per evaluation
    VG_CHECK((*rule)(f.ctx) != nullptr);
    VG_CHECK((*rule)(f.ctx) != nullptr);
    VG_CHECK((*rule)(f.ctx) == nullptr);
}

VG_TEST_CASE(tty_rule_reads_more_than_its_buffer_in_one_evaluation)
{
    TTYFixture f(/* buffer_size = */ 4);
    const auto rule = f.rule("STOP");
    f.peer.send("..........STOP"); // several four-byte reads within one evaluation
    VG_CHECK((*rule)(f.ctx) != nullptr);
    VG_CHECK((*rule)(f.ctx) == nullptr);
}

VG_TEST_CASE(sound_event_rule_config_rejects_a_negative_window_and_malformed_filters)
{
    SEDFixture f;
    const SoundEventRuleConfig cfg(SEDJson("head-1", 1000, { "doorbell", "baby_cry" }, 0.25f, 0.75f), f.ctx);
    VG_CHECK_EQ(cfg.id, "head-1");
    VG_CHECK(cfg.consecutive_window == std::chrono::milliseconds(1000));
    VG_CHECK_EQ(cfg.class_filter.size(), size_t { 2 });
    nlohmann::json out;
    cfg.to(out, f.ctx);
    VG_CHECK_EQ(SoundEventRuleConfig(out, f.ctx) == cfg, true); // round trip
    VG_CHECK_THROWS(SoundEventRuleConfig(SEDJson("h", -1, { "doorbell" }), f.ctx));
    VG_CHECK_THROWS(SoundEventRuleConfig(SEDJson("h", 0, { "doorbell" }, 0.9f, 0.1f), f.ctx));
    VG_CHECK_THROWS(SoundEventRuleConfig(SEDJson("h", 0, { "doorbell" }, 0.0f, 1.5f), f.ctx));
    auto class_not_array = SEDJson("h", 0, { "doorbell" });
    class_not_array["lClassFilter"] = "doorbell";
    VG_CHECK_THROWS(SoundEventRuleConfig(class_not_array, f.ctx));
    auto missing_window = SEDJson("h", 0, { "doorbell" });
    missing_window.erase("iConsecutiveWindowMs");
    VG_CHECK_THROWS(SoundEventRuleConfig(missing_window, f.ctx));
}

VG_TEST_CASE(sound_event_rule_filters_frames_and_an_empty_class_filter_matches_no_label)
{
    // The frame filter as implemented (docs/PLAN.md section 5.2): an envelope without an inference payload is
    // ignored, the head id must match when the rule names one, a top-k entry activates when its label is in the
    // class filter and its probability within the confidence range, and an empty class filter matches no label
    // at all (documented as "any label" before 2026-10-20; kept as implemented, decided 2026-10-19).
    SEDFixture f;
    const auto rule = f.unconnectedRule(SEDJson("head-1", 0, { "doorbell" }, 0.5f, 0.8f));
    const auto &access = *rule;
    VG_CHECK(SoundEventRuleTestAccess::filter(access, AudioEnvelope()) == Status::Ignored);
    VG_CHECK(SoundEventRuleTestAccess::filter(access, std::string()) == Status::Ignored); // an empty envelope
    VG_CHECK(SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "doorbell", 0.6f)) == Status::Active);
    VG_CHECK(SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "doorbell", 0.5f)) == Status::Active);
    VG_CHECK(SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "doorbell", 0.8f)) == Status::Active);
    VG_CHECK(
        SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "doorbell", 0.49f)) == Status::Inactive);
    VG_CHECK(
        SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "doorbell", 0.81f)) == Status::Inactive);
    VG_CHECK(SoundEventRuleTestAccess::filter(access,
                 InferenceEnvelope("head-1", "doorbell", std::numeric_limits<float>::quiet_NaN()))
             == Status::Inactive);
    VG_CHECK(SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-1", "cat", 0.6f)) == Status::Inactive);
    VG_CHECK(
        SoundEventRuleTestAccess::filter(access, InferenceEnvelope("head-2", "doorbell", 0.6f)) == Status::Inactive);
    VG_CHECK(SoundEventRuleTestAccess::filter(access, InferenceEnvelope(std::nullopt, "doorbell", 0.6f))
             == Status::Inactive); // a frame without a head id does not match a rule that names one
    VG_CHECK(SoundEventRuleTestAccess::filter(access, kMalformedEnvelope) == Status::Malformed);

    const auto any_head = f.unconnectedRule(SEDJson("", 0, { "doorbell" }));
    VG_CHECK(
        SoundEventRuleTestAccess::filter(*any_head, InferenceEnvelope("head-2", "doorbell", 0.9f)) == Status::Active);
    VG_CHECK(SoundEventRuleTestAccess::filter(*any_head, InferenceEnvelope(std::nullopt, "doorbell", 0.9f))
             == Status::Active);

    const auto no_label = f.unconnectedRule(SEDJson("", 0, {}));
    VG_CHECK(
        SoundEventRuleTestAccess::filter(*no_label, InferenceEnvelope("head-1", "doorbell", 0.9f)) == Status::Inactive);
    VG_CHECK(SoundEventRuleTestAccess::filter(*no_label, InferenceEnvelope("head-1", "", 0.9f)) == Status::Inactive);
}

VG_TEST_CASE(sound_event_rule_activates_once_a_run_of_active_frames_spans_the_window)
{
    // The consecutive window as implemented: a run of active frames activates on the first frame at least the
    // window after the run began, and on every active frame after that; an inactive frame ends the run, and
    // nothing happens without frames (the window is measured on the frames, not by a timer). With a window of
    // zero every active frame activates.
    SEDFixture f;
    const auto rule = f.unconnectedRule(SEDJson("", 1000, { "doorbell" }));
    std::optional<Clock::time_point> since;
    const auto t0 = Clock::now();
    const auto ms = [t0](int n) { return t0 + std::chrono::milliseconds(n); };
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(0)));
    VG_CHECK(since == ms(0));
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(999)));
    VG_CHECK(SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(1000)));
    VG_CHECK(SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(1001)));
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*rule, false, since, ms(1002))); // the run ends
    VG_CHECK(!since);
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(5000))); // a new run begins
    VG_CHECK(since == ms(5000));
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(5999)));
    VG_CHECK(SoundEventRuleTestAccess::finalActive(*rule, true, since, ms(6000)));

    const auto immediate = f.unconnectedRule(SEDJson("", 0, { "doorbell" }));
    since.reset();
    VG_CHECK(SoundEventRuleTestAccess::finalActive(*immediate, true, since, ms(0)));
    VG_CHECK(!SoundEventRuleTestAccess::finalActive(*immediate, false, since, ms(1)));
    VG_CHECK(SoundEventRuleTestAccess::finalActive(*immediate, true, since, ms(2)));
}

VG_TEST_CASE(sound_event_rule_latches_one_activation_until_evaluated_and_reconnects_with_backoff)
{
    // The poll loop over a fake result socket, as implemented: an activation is latched until an evaluation
    // consumes it, whenever that is (the rule loop evaluates the rule only while rules are enabled and the
    // schedule is open, so an activation during a closure is reported once after reopening), and several
    // activations before an evaluation yield one owner; ignored and inactive frames leave the latch alone; a
    // zero or oversized frame length and a malformed payload close the connection; the rule reconnects after a
    // backoff that doubles from 100 ms to 5 s across connections that delivered no valid frame and resets once
    // one did. The lower bounds on the gaps are the backoffs the rule sleeps; the one upper bound (the reset)
    // leaves a 700 ms margin to the next doubling.
    SEDFixture f;
    const auto rule = f.rule(SEDJson("", 0, { "doorbell" }));
    const auto active = InferenceEnvelope("head-1", "doorbell", 0.9f);
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK((*rule)(f.ctx) == nullptr);

    // Connection 1: an ignored frame, then an active one, latched and consumed once.
    f.peer.sendFrame(AudioEnvelope());
    f.peer.sendFrame(active);
    auto owner = f.latched(*rule);
    VG_REQUIRE(owner != nullptr);
    VG_CHECK_EQ(owner->type, VG_RULE_TYPE_SED);
    VG_CHECK_EQ(owner->id, "");
    VG_CHECK_EQ(owner->source_id, "");
    VG_CHECK_EQ(owner->source_delivery, EventOwner::SourceDelivery::Event);
    VG_CHECK((*rule)(f.ctx) == nullptr);
    // Two activations, an inactive frame, an ignored one, then a zero frame length, which closes the
    // connection; the close orders everything before it, so the latch holds exactly one activation now.
    f.peer.sendFrame(active);
    f.peer.sendFrame(active);
    f.peer.sendFrame(InferenceEnvelope("head-1", "cat", 0.9f));
    f.peer.sendFrame(AudioEnvelope());
    f.peer.send(SocketPeer::Prefix(0));
    VG_REQUIRE(f.peer.waitForClose(kWait));
    auto closed = Clock::now();
    VG_CHECK((*rule)(f.ctx) != nullptr);
    VG_CHECK((*rule)(f.ctx) == nullptr);

    // Connection 2, 100 ms later (the previous one delivered valid frames): a malformed payload closes it.
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK(Clock::now() - closed >= std::chrono::milliseconds(100));
    f.peer.sendFrame(active);
    VG_CHECK(f.latched(*rule) != nullptr);
    f.peer.sendFrame(kMalformedEnvelope);
    VG_REQUIRE(f.peer.waitForClose(kWait));
    closed = Clock::now();

    // Connection 3: an oversized frame length closes it before any payload.
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK(Clock::now() - closed >= std::chrono::milliseconds(100));
    f.peer.sendFrame(active);
    VG_CHECK(f.latched(*rule) != nullptr);
    f.peer.send(SocketPeer::Prefix(64 * 1024 + 1));
    VG_REQUIRE(f.peer.waitForClose(kWait));
    closed = Clock::now();

    // Connections 4 and 5 deliver nothing before the peer closes them: the backoff doubles, 200 ms then 400 ms.
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK(Clock::now() - closed >= std::chrono::milliseconds(100));
    f.peer.disconnect();
    closed = Clock::now();
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK(Clock::now() - closed >= std::chrono::milliseconds(200));
    f.peer.disconnect();
    closed = Clock::now();
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK(Clock::now() - closed >= std::chrono::milliseconds(400));

    // Connection 6 delivers a valid frame before the peer closes it: the backoff is back to 100 ms.
    f.peer.sendFrame(active);
    VG_CHECK(f.latched(*rule) != nullptr);
    f.peer.disconnect();
    closed = Clock::now();
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    const auto gap = Clock::now() - closed;
    VG_CHECK(gap >= std::chrono::milliseconds(100));
    VG_CHECK(gap < std::chrono::milliseconds(800));

    // Connection 7: an activation latched before the close is still there after the reconnect, unevaluated.
    f.peer.sendFrame(active);
    f.peer.send(SocketPeer::Prefix(0));
    VG_REQUIRE(f.peer.waitForClose(kWait));
    VG_REQUIRE(f.peer.acceptWithin(kWait));
    VG_CHECK((*rule)(f.ctx) != nullptr);
    VG_CHECK((*rule)(f.ctx) == nullptr);
}

VG_TEST_CASE(sound_event_rule_stops_promptly_while_waiting_for_a_peer_or_a_frame)
{
    // Destruction breaks every wait of the poll loop within its 100 ms poll interval: the reconnect backoff
    // with nobody listening, the wait for a frame prefix (which has no deadline), and the read of a frame
    // that stopped halfway (which has a 5 s deadline).
    SEDFixture f;
    {
        auto rule = f.unconnectedRule(SEDJson("", 0, { "doorbell" }));
        const auto before = Clock::now();
        rule.reset();
        VG_CHECK(Clock::now() - before < std::chrono::seconds(2));
    }
    {
        auto rule = f.rule(SEDJson("", 0, { "doorbell" }));
        VG_REQUIRE(f.peer.acceptWithin(kWait));
        const auto before = Clock::now();
        rule.reset();
        VG_CHECK(Clock::now() - before < std::chrono::seconds(2));
    }
    {
        auto rule = f.rule(SEDJson("", 0, { "doorbell" }));
        VG_REQUIRE(f.peer.acceptWithin(kWait));
        f.peer.send(SocketPeer::Prefix(16).substr(0, 2)); // half a prefix, then silence
        const auto before = Clock::now();
        rule.reset();
        VG_CHECK(Clock::now() - before < std::chrono::seconds(2));
    }
}

VG_TEST_MAIN()
