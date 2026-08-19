#pragma once
#include <vector>
#include <stream/BitReader.h>
#include <stream/StreamReader.h>
#include <structures/FileStructures.h>
#include <extensions/FlagExtensions.h>

namespace BlizzardDatabaseLib {
    namespace Reader {

        class WDBCRecordReader
        {        
            Structures::WDC3Header& _fileHeader;
            Stream::BitReader& _bitReader;
            Structures::VersionDefinition& _versionDefinition;

            std::shared_ptr<Stream::StreamReader> _streamReader;
        public:
            WDBCRecordReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, Stream::BitReader& bitReader, Structures::WDC3Header& fileHeader);
           
            Structures::BlizzardDatabaseRow ReadRecord(int indexOfId, Stream::BitReader& reader);
        };
    }
}