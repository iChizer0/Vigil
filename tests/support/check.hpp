#pragma once
#ifndef VIGIL_TESTS_CHECK_HPP
#define VIGIL_TESTS_CHECK_HPP

// Minimal test support for the module-owned tests. There is no external test framework: a test
// executable registers cases with VG_TEST_CASE, asserts with VG_CHECK / VG_CHECK_EQ / VG_REQUIRE /
// VG_CHECK_THROWS, and ends with VG_TEST_MAIN. Every failed check prints "file:line: ..." and makes
// the process exit non-zero, which is what CTest reports.

#include <cstddef>
#include <cstdio>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

namespace vigil::tests {

struct RequireFailed: public std::exception
{
    const char *what() const noexcept override
    {
        return "required check failed";
    }
};

class Registry final
{
public:
    using Function = void (*)();

    struct Case
    {
        const char *name;
        Function fn;
    };

    static Registry &instance() noexcept
    {
        static Registry registry;
        return registry;
    }

    void add(const char *name, Function fn)
    {
        _cases.push_back(Case { name, fn });
    }

    void fail(const char *file, int line, const std::string &what) noexcept
    {
        ++_failures;
        std::fprintf(stderr, "%s:%d: %s\n", file, line, what.c_str());
    }

    int runAll() noexcept
    {
        size_t failed_cases = 0;
        for (const auto &c: _cases)
        {
            const auto before = _failures;
            try
            {
                c.fn();
            }
            catch (const RequireFailed &)
            {
            }
            catch (const std::exception &e)
            {
                fail(c.name, 0, std::string("unexpected exception: ") + e.what());
            }
            catch (...)
            {
                fail(c.name, 0, "unexpected non-standard exception");
            }
            const bool ok = _failures == before;
            failed_cases += ok ? 0 : 1;
            std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
        }
        std::printf("%zu case(s), %zu failed\n", _cases.size(), failed_cases);
        return failed_cases == 0 ? 0 : 1;
    }

private:
    std::vector<Case> _cases;
    size_t _failures = 0;
};

struct Registrar
{
    Registrar(const char *name, Registry::Function fn)
    {
        Registry::instance().add(name, fn);
    }
};

template<typename T>
std::string Describe(const T &value)
{
    std::ostringstream oss;
    oss << value;
    return oss.str();
}

inline std::string Describe(bool value)
{
    return value ? "true" : "false";
}

inline std::string Describe(const char *value)
{
    return value ? "\"" + std::string(value) + "\"" : std::string("<null>");
}

inline std::string Describe(const std::string &value)
{
    return "\"" + value + "\"";
}

} // namespace vigil::tests

#define VG_TEST_CASE(name)                                                                                             \
    static void vg_test_##name();                                                                                      \
    static const ::vigil::tests::Registrar vg_test_registrar_##name(#name, &vg_test_##name);                           \
    static void vg_test_##name()

#define VG_TEST_MAIN()                                                                                                 \
    int main()                                                                                                         \
    {                                                                                                                  \
        return ::vigil::tests::Registry::instance().runAll();                                                          \
    }

#define VG_CHECK(cond)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
            ::vigil::tests::Registry::instance().fail(__FILE__, __LINE__, "check failed: " #cond);                     \
    }                                                                                                                  \
    while (0)

#define VG_REQUIRE(cond)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            ::vigil::tests::Registry::instance().fail(__FILE__, __LINE__, "required check failed: " #cond);            \
            throw ::vigil::tests::RequireFailed();                                                                     \
        }                                                                                                              \
    }                                                                                                                  \
    while (0)

#define VG_CHECK_EQ(actual, expected)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        const auto &vg_actual_ = (actual);                                                                             \
        const auto &vg_expected_ = (expected);                                                                         \
        if (!(vg_actual_ == vg_expected_))                                                                             \
            ::vigil::tests::Registry::instance().fail(__FILE__, __LINE__,                                              \
                std::string("check failed: " #actual " == " #expected " (actual ")                                     \
                    + ::vigil::tests::Describe(vg_actual_) + ", expected " + ::vigil::tests::Describe(vg_expected_)    \
                    + ")");                                                                                            \
    }                                                                                                                  \
    while (0)

#define VG_CHECK_THROWS(expr)                                                                                          \
    do                                                                                                                 \
    {                                                                                                                  \
        bool vg_thrown_ = false;                                                                                       \
        try                                                                                                            \
        {                                                                                                              \
            (void)(expr);                                                                                              \
        }                                                                                                              \
        catch (...)                                                                                                    \
        {                                                                                                              \
            vg_thrown_ = true;                                                                                         \
        }                                                                                                              \
        if (!vg_thrown_)                                                                                               \
            ::vigil::tests::Registry::instance().fail(__FILE__, __LINE__, "expected an exception from: " #expr);       \
    }                                                                                                                  \
    while (0)

#endif
