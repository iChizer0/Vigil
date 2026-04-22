#pragma once
#ifndef VIGIL_JSON_HPP
#define VIGIL_JSON_HPP

#include <exception>
#include <stdexcept>
#include <string>

#define JSON_DIAGNOSTIC_POSITIONS 1

class CleanJsonException: public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

#define JSON_THROW_USER(exception)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        std::string msg = (exception).what();                                                                          \
        auto pos = msg.find_first_of("] ");                                                                            \
        if (pos != std::string::npos)                                                                                  \
        {                                                                                                              \
            msg = msg.substr(pos + 2);                                                                                 \
        }                                                                                                              \
        throw CleanJsonException(msg);                                                                                 \
    }                                                                                                                  \
    while (0)

#include <nlohmann/json.hpp>

#endif
