#pragma once

#include <optional>
#include <string>

namespace Platform {

    // Environment: access to the process environment variables. Hides the
    // compiler-specific functions behind one portable call.
    namespace Environment {

        // Returns the value of an environment variable, or std::nullopt if
        // it is not set. A variable set to an empty string returns "".
        std::optional<std::string> Get_variable(const char* _name);

    }

}