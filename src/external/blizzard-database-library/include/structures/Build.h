#pragma once
#include <string>
#include <extensions/StringExtensions.h>

namespace BlizzardDatabaseLib {
    namespace Structures {

        enum class DatabaseFormat
        {
            WDBC,
            WDC3
        };

        class Build
        {
        private:
            short _expansion;
            short _major;
            short _minor;
            unsigned int build;
        public:
            Build(const std::string& buildString)
            {
                auto delimited = std::string(".");
                auto buildVersionElements = Extension::String::Split(buildString, delimited);

                _expansion = std::atoi(buildVersionElements[0].c_str());
                _major = std::atoi(buildVersionElements[1].c_str());
                _minor = std::atoi(buildVersionElements[2].c_str());
                build = std::atoi(buildVersionElements[3].c_str());
            }

            inline unsigned int buildId() const { return build; };

            inline bool operator==(const Build& rhs) const {
                return std::tie(_expansion, _major, _minor, build) == std::tie(rhs._expansion, rhs._major, rhs._minor, rhs.build);
            }

            inline bool operator<(const Build& rhs) const {
                return std::tie(_expansion, _major, _minor, build) < std::tie(rhs._expansion, rhs._major, rhs._minor, rhs.build);
            }

            inline bool operator>(const Build& rhs) const {
                return std::tie(_expansion, _major, _minor, build) > std::tie(rhs._expansion, rhs._major, rhs._minor, rhs.build);
            }
        };
    }
}
