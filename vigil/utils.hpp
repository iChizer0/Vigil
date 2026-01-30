#pragma once
#ifndef VIGIL_UTILS_HPP
#define VIGIL_UTILS_HPP

#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace vigil::utils {

class Execute
{
public:
    Execute(std::string program, std::vector<std::string> args) : _program(std::move(program)), _args(std::move(args))
    {
        _argv.reserve(_args.size() + 2);
        _argv.push_back(const_cast<char *>(_program.c_str()));
        for (const auto &arg: _args)
            _argv.push_back(const_cast<char *>(arg.c_str()));
        _argv.push_back(nullptr);
    }

    Execute(const Execute &) = delete;
    Execute &operator=(const Execute &) = delete;

    Execute(Execute &&) = delete;
    Execute &operator=(Execute &&) = delete;

    ~Execute() = default;

    [[nodiscard]] int operator()() const
    {
        pid_t pid = ::fork();
        if (pid == -1)
        {
            return -1;
        }
        else if (pid == 0)
        {
            int dev_null = ::open("/dev/null", O_WRONLY);
            if (dev_null == -1)
            {
                ::_exit(1);
            }
            ::dup2(dev_null, STDOUT_FILENO);
            ::dup2(dev_null, STDERR_FILENO);
            if (dev_null > STDERR_FILENO)
                ::close(dev_null);

            ::execvp(_argv[0], _argv.data());

            ::_exit(127);
        }
        else
        {
            int status;
            while (::waitpid(pid, &status, 0) == -1)
            {
                if (errno != EINTR)
                {
                    return -1;
                }
            }

            if (WIFEXITED(status))
            {
                return WEXITSTATUS(status);
            }
            else
            {
                return -1;
            }
        }
    }

private:
    std::string _program;
    std::vector<std::string> _args;
    mutable std::vector<char *> _argv;
};

} // namespace vigil::utils

#endif
