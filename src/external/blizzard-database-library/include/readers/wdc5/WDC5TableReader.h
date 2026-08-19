#pragma once
#include <type_traits>
#include <fstream>
#include <istream>
#include <iostream>
#include <vector>
#include <map>
#include <sstream>
#include <flags/TableFormatSignatures.h>
#include <readers/IBlizzardTableReader.h>
#include <stream/StreamReader.h>
#include <stream/BitReader.h>
#include <structures/Types.h>
#include <structures/FileStructures.h>
#include <extensions/StringExtensions.h>
#include <extensions/FlagExtensions.h>
#include <readers/wdc5/WDC5RecordReader.h>
#include <readers/wdc5/WDC5SectionReader.h>

#include <extensions/MemoryExtensions.h>
#include <extensions/VectorExtensions.h>


namespace BlizzardDatabaseLib {
    namespace Reader {

        class WDC5TableReader : public IBlizzardTableReader
        {
            std::shared_ptr<Stream::StreamReader> _streamReader;

            Structures::WDC5Header Header;
            std::vector<Structures::WDC5Section> Sections;
            std::vector<Structures::FieldMeta> Meta;
            std::vector<Structures::ColumnMetaData> ColumnMeta;
            std::map<int, std::vector<Structures::Int32>> PalletData;
            std::map<int, std::map<int, Structures::Int32>> CommonData;
            std::map<int, std::shared_ptr<WDC5SectionReader>> _sectionLookup;
            Structures::VersionDefinition _versionDefinition;

            unsigned int _sectionMaxIndexCounter = 0;
        public:
            WDC5TableReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition versionDefinition);
            ~WDC5TableReader();
            void LoadTableStructure() override;
            void CloseAllSections() override;
            Structures::BlizzardDatabaseRow RecordById(unsigned int Id) override;
            Structures::BlizzardDatabaseRow Record(unsigned int index) override;
            Structures::BlizzardDatabaseRowDefinition RecordDefinition() override;
            std::size_t RecordCount() override;
            std::size_t FieldCount() override;
        };
    }
}