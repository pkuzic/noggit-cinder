#pragma once
#include <iostream>
#include <vector>
#include <map>
#include <sstream>
#include <flags/TableFormatSignatures.h>
#include <readers/IBlizzardTableReader.h>
#include <readers/wdc3/WDC3RecordReader.h>
#include <stream/StreamReader.h>
#include <stream/BitReader.h>
#include <structures/Types.h>
#include <structures/FileStructures.h>
#include <extensions/StringExtensions.h>
#include <extensions/FlagExtensions.h>
#include <extensions/MemoryExtensions.h>
#include <extensions/VectorExtensions.h>

namespace BlizzardDatabaseLib {
    namespace Reader {

        class WDC3SectionReader
        {
        public:
            bool IsOpen = false;
            Structures::WDC3Section Section;
            std::vector<int> IndexData;
            std::vector<Structures::SparseEntry> SparseEntryData;
            Structures::ReferenceData ReferenceData;
            std::map<int, int> CopyData;
        private:
            int _sectionStartoffset;
            int _recordCount;

            Structures::WDC3Header& _header;
            std::shared_ptr<Stream::StreamReader> _streamReader;
            std::shared_ptr<char[]> _sectionDataBlock;
        public:
            WDC3SectionReader(std::shared_ptr<Stream::StreamReader> _streamReader, Structures::WDC3Section section, Structures::WDC3Header& _header);
            std::shared_ptr<char[]> GetSection();
            std::shared_ptr<char[]> OpenSection();
            void CloseSection();
        };
    }
}