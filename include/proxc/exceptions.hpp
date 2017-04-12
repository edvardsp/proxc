
#pragma once

#include <string>
#include <system_error>

#include <proxc/config.hpp>

PROXC_NAMESPACE_BEGIN

class UnreachableError : public std::system_error
{
public:
    UnreachableError()
        : std::system_error{ std::make_error_code( std::errc::state_not_recoverable ), "unreachable" }
    {}

    UnreachableError( std::error_code ec )
        : std::system_error{ ec }
    {}

    UnreachableError( std::error_code ec, const char * what_arg )
        : std::system_error{ ec, what_arg }
    {}

    UnreachableError( std::error_code ec, std::string const & what_arg )
        : std::system_error{ ec, what_arg }
    {}

    ~UnreachableError() = default;
};

PROXC_NAMESPACE_END

