#include <readers/wdbc/WDBCRecordReader.h>

namespace BlizzardDatabaseLib {
    namespace Reader {
        WDBCRecordReader::WDBCRecordReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, Stream::BitReader& bitReader, Structures::WDC3Header& fileHeader)
            : _bitReader(bitReader), _streamReader(streamReader), _versionDefinition(versionDefinition), _fileHeader(fileHeader)
        {

        }

        Structures::BlizzardDatabaseRow WDBCRecordReader::ReadRecord(int indexOfId,  Stream::BitReader& reader)
        {

            return Structures::BlizzardDatabaseRow();
        }
    }
}