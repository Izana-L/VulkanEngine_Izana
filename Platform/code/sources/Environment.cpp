#include "Environment.hpp"
#include <cstdlib>

namespace Platform {
    namespace Environment {

        std::optional<std::string> Get_variable(const char* _name) {
#ifdef _MSC_VER
            // MSVC flags std::getenv as unsafe (C4996, an error with SDL
            // checks enabled). _dupenv_s is its checked replacement; it
            // returns a heap copy that has to be released with free().
            char* value = nullptr;
            size_t length = 0;
            std::optional<std::string> result;
            if (_dupenv_s(&value, &length, _name) == 0 && value != nullptr) {
                result = value;
            }
            std::free(value);
            return result;
#else
            const char* value = std::getenv(_name);
            if (value == nullptr) return std::nullopt;
            return std::string(value);
#endif
        }

    }
}