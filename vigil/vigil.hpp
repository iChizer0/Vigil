#pragma once
#ifndef VIGIL_HPP
#define VIGIL_HPP

#include <memory>

#include "endpoints.hpp"
#include "storage.hpp"
#include "vigil_ctypes.h"

namespace vigil {

using namespace vigil::endpoints;

class Vigil final
{
public:
    Vigil(const vg_config_t &cfg)
        : _context(std::make_shared<Context>(cfg)), _rule_endpoint(std::make_unique<RuleEndpoint>(_context)),
          _storage_endpoint(std::make_unique<StorageEndpoint>(_context))
    {
    }

    ~Vigil() = default;

    Context &context() noexcept
    {
        return *_context;
    }

    RuleEndpoint &ruleEndpoint() noexcept
    {
        return *_rule_endpoint;
    }

    StorageEndpoint &storageEndpoint() noexcept
    {
        return *_storage_endpoint;
    }

private:
    std::shared_ptr<Context> _context;
    std::unique_ptr<RuleEndpoint> _rule_endpoint;
    std::unique_ptr<StorageEndpoint> _storage_endpoint;
};

} // namespace vigil

#endif
