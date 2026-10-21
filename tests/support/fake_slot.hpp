#pragma once
#ifndef VIGIL_TESTS_FAKE_SLOT_HPP
#define VIGIL_TESTS_FAKE_SLOT_HPP

// The Slot admission that tests register in place of a storage Slot (docs/PLAN.md section 8.1): ready unless
// told otherwise, with a fake filesystem's free space for the budget. Admission opens on construction and
// closes on destruction, as a Slot's does on enable and disable; `close` and `open` do the same in between.

#include <atomic>
#include <cstdint>
#include <limits>

#include "vigil/context.hpp"
#include "vigil/types.hpp"

namespace vigil::tests {

using namespace vigil::types;

struct FakeSlotAdmission: Context::SlotAdmission
{
    static constexpr Context::InFlightBudgets::FilesystemId kFilesystem = 42;

    Context &ctx;
    const Context::InFlightBudgets::FilesystemId filesystem; // the fake filesystem the budget is kept for
    std::atomic<bool> ready = true;
    std::atomic<FileSize> free_bytes = std::numeric_limits<FileSize>::max() / 2;

    // `filesystem` is a real filesystem's id when a test shares the budget with a manual capture there.
    explicit FakeSlotAdmission(Context &c, Context::InFlightBudgets::FilesystemId fs = kFilesystem)
        : ctx(c), filesystem(fs)
    {
        open();
    }

    ~FakeSlotAdmission() override
    {
        close();
    }

    // Opens admission: the tasks accepted from here on bind to a new registration.
    void open()
    {
        _registration.store(ctx.slot_admission.open(*this));
    }

    // Closes admission: the pending tasks bound to the registration are canceled.
    void close()
    {
        if (const auto registration = _registration.exchange(0))
            ctx.slot_admission.close(registration);
    }

    // The open registration, 0 while closed: what a worker claims by.
    uint64_t registration() const noexcept
    {
        return _registration.load();
    }

    bool isReadyToWrite() const noexcept override
    {
        return ready.load();
    }

    Context::InFlightBudgets::Reservation tryAdmit(const WaterMarks &admission, FileSize task_bytes) noexcept override
    {
        return ctx.in_flight_budgets.tryReserve(filesystem, task_bytes, free_bytes.load(),
            admission.critical_remain_bytes);
    }

private:
    std::atomic<uint64_t> _registration = 0;
};

} // namespace vigil::tests

#endif
