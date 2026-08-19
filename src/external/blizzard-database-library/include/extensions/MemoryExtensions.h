#pragma once
#include <string>
#include <sstream>

namespace BlizzardDatabaseLib {
    namespace Extension {

        class Memory
        {
        public:
            bool static IsEmpty(char* data, size_t length)
            {
                return memcmp(data, data + 1, length - 1) == 0;
            }

            static void Dump(const char* mem, size_t length) {
                std::cout << std::setfill('0');
                for (size_t i = 0; i < length; ++i) {
                    std::cout << std::hex << std::setw(2) << (int)mem[i];
                	std::cout << (((i + 1) % 8 == 0) ? "\n" : " ");
                }
                std::cout << std::endl;
            }
        };
    }
}