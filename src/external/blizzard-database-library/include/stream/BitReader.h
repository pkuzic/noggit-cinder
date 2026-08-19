#pragma once
#include <memory>
#include <cstring>
#include <string>
#include <sstream>
#include <structures/Types.h>

namespace BlizzardDatabaseLib {
    namespace Stream {

        class BitReader
        {
            std::unique_ptr<char[]>& _dataStart;
        public:
            int Position;
            int DataLength;
            int Offset;
            BitReader(std::unique_ptr<char[]>& dataStart, unsigned int dataLength);
            std::string ReadNullTermintingString();
            int ReadInt32(int numberOfBits);
            unsigned int ReadUint32(int numberOfBits);
            unsigned long long ReadUint64(int numberOfBits);
            Structures::Int64 ReadValue64(int numberOfBits);
            Structures::Int64 ReadSignedValue64(int numberOfBits);
            void Reset(int offset = 0) { Position = 0; Offset = offset; }
        };

    }
}
