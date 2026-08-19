#pragma once
#include <cstring>
#include <vector>
#include <string>
#include <sstream>
#include <structures/Types.h>
#include <flags/Flags.h>

namespace BlizzardDatabaseLib {
    namespace Extension {

        class Flag
        {
        public:
            inline static bool HasFlag(const BlizzardDatabaseLib::Flag::DatabaseVersion2Flag& lhs, const BlizzardDatabaseLib::Flag::DatabaseVersion2Flag& rhs) noexcept
            {
                return (lhs & rhs) == rhs;
            }
        };
    }
}