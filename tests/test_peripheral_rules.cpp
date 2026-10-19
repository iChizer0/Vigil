// Characterization tests for the GPIO and TTY rules over fake peripherals (docs/PLAN.md sections 5.2 and 6.1):
// the sync callback the selected GPIO signal mode needs, level and edge evaluation, the TTY command matcher
// (overlapping prefixes, commands split across reads, several commands in one read, reads larger than the
// buffer), and the owner attribution both rules carry. The TTY peer is a Unix socket in a temporary directory.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

// A listening Unix socket the TTY rule connects to.
struct TTYPeer
{
    std::string dir;
    std::string path;
    int listen_fd = -1;
    int peer_fd = -1;

    TTYPeer()
    {
        std::string p = (std::filesystem::temp_directory_path() / "vigil-test-XXXXXX").string();
        if (::mkdtemp(p.data()) == nullptr)
            throw std::runtime_error("mkdtemp failed");
        dir = p;
        path = dir + "/tty.sock";
        listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr {};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (listen_fd < 0 || ::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0
            || ::listen(listen_fd, 1) != 0)
            throw std::runtime_error("failed to listen on " + path);
    }

    ~TTYPeer()
    {
        if (peer_fd >= 0)
            ::close(peer_fd);
        if (listen_fd >= 0)
            ::close(listen_fd);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    // Accepts the connection the rule made in its constructor.
    void accept()
    {
        peer_fd = ::accept(listen_fd, nullptr, nullptr);
        if (peer_fd < 0)
            throw std::runtime_error("accept failed");
    }

    // Bytes sent on a Unix stream socket are readable by the peer when this returns.
    void send(const std::string &bytes)
    {
        if (::send(peer_fd, bytes.data(), bytes.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(bytes.size()))
            throw std::runtime_error("send failed");
    }
};

struct TTYFixture
{
    TTYPeer peer;
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

VG_TEST_MAIN()
