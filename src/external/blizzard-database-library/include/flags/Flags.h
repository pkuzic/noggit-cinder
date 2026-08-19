#pragma once
#include <type_traits>

namespace BlizzardDatabaseLib {
    namespace Flag {

        enum class DatabaseVersion2Flag : unsigned short
        {
            None = 0x0,
            VariableWidthRecord = 0x1,
            SecondaryKey = 0x2,
            Index = 0x4,
            Unknown1 = 0x8,
            BitPacked = 0x10
        };

        inline DatabaseVersion2Flag operator& (DatabaseVersion2Flag lhs, DatabaseVersion2Flag rhs)
        {
            using T = std::underlying_type_t<DatabaseVersion2Flag>;
            return static_cast<DatabaseVersion2Flag>(static_cast<T>(lhs) & static_cast<T>(rhs));
        }
    }
}