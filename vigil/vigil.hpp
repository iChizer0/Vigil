#pragma once
#ifndef VIGIL_HPP
#define VIGIL_HPP

#include <memory>

#include "endpoints.hpp"
#include "vigil_ctypes.h"

namespace vigil {

using namespace vigil::endpoints;

class Vigil final
{
public:
    Vigil(const vg_config_t &cfg)
        : _context(std::make_shared<Context>(cfg)), _rule_endpoint(std::make_unique<RuleEndpoint>(_context)),
          _storage_endpoint(std::make_unique<StorageEndpoint>(_context)),
          _capture_endpoint(std::make_unique<CaptureEndpoint>(_context))
    {
    }

    // Stops in dependency order (docs/PLAN.md section 8.3): the rule producer first, so nothing new is
    // dispatched while the workers drain; then the pending file tasks are canceled and the Sound worker is
    // stopped (a pending trigger is canceled, a playing callback finishes); then the manual capture worker and the
    // storage slots, whose destructors interrupt, finish, and publish admitted work before joining; the context,
    // which every worker and callback uses, goes last. A constructor that throws destroys the members built so
    // far, so a partial initialization still joins every thread it started.
    ~Vigil() noexcept
    {
        _rule_endpoint.reset();
        _context->file_channels.cancelPending();
        _context->sound_executor.stopAndJoin();
        _capture_endpoint.reset();
        _storage_endpoint.reset();
    }

    Context &context() noexcept
    {
        return *_context;
    }

    std::shared_ptr<Context> contextPtr() const noexcept
    {
        return _context;
    }

    RuleEndpoint &ruleEndpoint() noexcept
    {
        return *_rule_endpoint;
    }

    StorageEndpoint &storageEndpoint() noexcept
    {
        return *_storage_endpoint;
    }

    CaptureEndpoint &captureEndpoint() noexcept
    {
        return *_capture_endpoint;
    }

private:
    std::shared_ptr<Context> _context;
    std::unique_ptr<RuleEndpoint> _rule_endpoint;
    std::unique_ptr<StorageEndpoint> _storage_endpoint;
    std::unique_ptr<CaptureEndpoint> _capture_endpoint;
};

} // namespace vigil

#endif
