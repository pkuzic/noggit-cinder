#pragma once
#include<structures/Build.h>

namespace BlizzardDatabaseLib {
    namespace Structures {

        class BuildRange
        {
        private:
            Build _minBuild;
            Build _maxBuild;
        public:
            BuildRange(Build minBuild, Build maxBuild) : _minBuild(minBuild), _maxBuild(maxBuild) {}

            bool Contains(const Build& build)
            {
                if (_minBuild < build && _maxBuild > build)
                    return true;
                if (_minBuild == build || _maxBuild == build)
                    return true;
                return false;
            }
        };
    }
}
