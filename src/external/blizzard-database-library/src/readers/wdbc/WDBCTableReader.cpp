#include <readers/wdbc/WDBCTableReader.h>
#include <extensions/MemoryExtensions.h>

#include <cassert>
#include <map>
#include <string>

namespace
{
    // Non-throwing string-table lookup. The WDBC reader assumes 16-locale locstrings, but
    // pre-TBC (vanilla) DBCs have 8 locales, so a locstring read over-runs the record and
    // hands a garbage offset to the string table. .at() would throw std::out_of_range and
    // (being uncaught) crash the whole app; return an empty string for unknown offsets instead.
    inline std::string safeStringAt(std::map<long, std::string> const& table, long key)
    {
        auto const it = table.find(key);
        return it != table.end() ? it->second : std::string();
    }
}

namespace BlizzardDatabaseLib {
    namespace Reader {
  
        WDBCTableReader::WDBCTableReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition versionDefinition)
          : _streamReader(streamReader), _versionDefinition(versionDefinition), Header{ 0, 0, 0, 0 }
        {
            _stringTable = std::map<long, std::string>();
        }

        WDBCTableReader::~WDBCTableReader()
        {
            _streamReader.reset();
        }


        void WDBCTableReader::LoadTableStructure()
        {
            auto length = _streamReader->Length();
            auto headerSize = sizeof(Structures::WDBCHeader);

            if (length < headerSize)
            {
                std::cout << "Error Occured While Parsing WDBC Header, Header to short." << std::endl;
                return;
            }

            _streamReader->Jump(0);

            auto magicNumber = _streamReader->Read<unsigned int>();
            if (magicNumber != Flag::TableFormatSignatures::WDBC_FMT_SIGNATURE)
            {
                std::cout << "Error Occured While Parsing WDBC Header, Format Signature doesnt Match." << std::endl;
                return;
            }

            Header = _streamReader->Read<Structures::WDBCHeader>();
            _recordData = _streamReader->ReadBlock(Header.RecordsCount * Header.RecordSize);
         
            for(int i = 0; i < Header.StringTableSize;)
            {
                auto lastPosition = _streamReader->Position();
                auto stringValue = _streamReader->ReadString();
                _stringTable[i] = stringValue;
                i += static_cast<int>(_streamReader->Position() - lastPosition);
            }
            assert(Header.RecordSize == (Header.FieldsCount * 4)); // Find out if there's any column that doesn't use 4 bytes in WDBC, in which case we need a rewrite
        }

        Structures::BlizzardDatabaseRow WDBCTableReader::RecordById(unsigned int Id)
        {
            assert(_versionDefinition.hasId); // attempt to get record by id on a table that doesn't have a key/id column. use Record() instead
            if (!_versionDefinition.hasId)
              return Structures::BlizzardDatabaseRow();

            int offset = 0;
            for(int i = 0; i < Header.RecordsCount; i++)
            {
                int id_col_size = _versionDefinition.versionDefinitions.definitions[_versionDefinition.idColumnIndex].size;
                assert(id_col_size == 32);

                // this assumes all columns have the same size as id column.
                // to be robust, if columns ever have different sizes and id wasn't at the beginning we would have to properly iterate the row
                int id_column_offset = offset + (id_col_size * _versionDefinition.idColumnIndex);

                auto recordDataBlock = std::make_unique<char[]>(id_col_size);
                memcpy(recordDataBlock.get(), _recordData.get() + id_column_offset, id_col_size);
                auto bitReader = Stream::BitReader(recordDataBlock, Header.RecordSize);

                auto rowId = bitReader.ReadUint32(id_col_size);
                if(rowId == Id)
                {
                    return Record(i);
                }

                offset += Header.RecordSize;
            }

            return Structures::BlizzardDatabaseRow();
        }

        Structures::BlizzardDatabaseRow WDBCTableReader::Record(unsigned int index)
        {
            auto recordDataOffset = Header.RecordSize * index;
            auto recordDataBlock = std::make_unique<char[]>(Header.RecordSize);
            memcpy(recordDataBlock.get(), _recordData.get() + recordDataOffset, Header.RecordSize);

            auto bitReader = Stream::BitReader(recordDataBlock, Header.RecordSize);

            // TODO : un-hardcode size if needed
            assert(_versionDefinition.idColumnIndex == 0); // for dev, find if there's ever any dbc that has id =/= column 0, in this case this is fucked
            unsigned int rowId = -1;
            int i = 0;
            if (_versionDefinition.hasId)
            {
              i = 1;
              int id_col_size = _versionDefinition.versionDefinitions.definitions[_versionDefinition.idColumnIndex].size;
              // int id_column_offset = offset + (id_col_size * _versionDefinition.idColumnIndex);
              rowId = bitReader.ReadUint32(id_col_size); // read id from first column
            }

            auto row = Structures::BlizzardDatabaseRow(rowId);
            for(i; i < _versionDefinition.versionDefinitions.definitions.size(); i++) // !!! skips first column, starts at i = 1.
            {
                auto& definition = _versionDefinition.versionDefinitions.definitions[i];
                auto& columnDefinition = _versionDefinition.columnDefinitions.at(definition.name);

                if (columnDefinition.type == "int")
                  assert(definition.size == 32); // Find out if there's any column that doesn't use 4 bytes in WDBC

                auto column = Structures::BlizzardDatabaseColumn();

                auto value = std::string();
                if (Extension::String::Compare(columnDefinition.type, "int"))
                {
                    if (definition.arrLength > 1)
                    {
                        column.Values.reserve(definition.arrLength);
                        for (int i = 0; i < definition.arrLength; i++)
                        {
                            // auto intValue = bitReader.ReadSignedValue64(32).As<int>();
                            int intValue = (bitReader.ReadInt32(32));
                            column.Values.push_back(std::to_string(intValue));
                        }
                    }
                    else
                    {
                        // auto intValue = bitReader.ReadSignedValue64(32).As<int>();
                        int intValue = (bitReader.ReadInt32(32));
                        value = std::to_string(intValue);
                    }
                }

                else if (Extension::String::Compare(columnDefinition.type, "string"))
                {
                    if (definition.arrLength > 1)
                    {
                        column.Values.reserve(definition.arrLength);
                        for (int i = 0; i < definition.arrLength; i++)
                        {
                            auto intValue = bitReader.ReadUint32(32);
                            column.Values.push_back(safeStringAt(_stringTable, intValue));
                        }
                    }
                    else
                    {
                        auto intValue = bitReader.ReadUint32(32);
                        value = safeStringAt(_stringTable, intValue);
                    }
                }

                else if (Extension::String::Compare(columnDefinition.type, "locstring"))
                {
                    auto intValue = bitReader.ReadUint32(32);
                    column.Value = safeStringAt(_stringTable, intValue);

                    column.Values.reserve(16);
                    column.Values.push_back(column.Value); // optional : store the first(enUS) value as single Value too. TODO: This doesn't support multi locales
                    for(int i = 0 ; i < 15; i++)
                    {
                        intValue = bitReader.ReadUint32(32);
                        column.Values.push_back(safeStringAt(_stringTable, intValue));
                    }
                    row.Columns[definition.name] = column;

                    column = Structures::BlizzardDatabaseColumn();
                    intValue = bitReader.ReadUint32(32);
                    column.Value = std::to_string(intValue);
                    row.Columns[definition.name + "_flags"] = column;
                    continue;
                }

                else if (Extension::String::Compare(columnDefinition.type, "float"))
                {
                    if (definition.arrLength > 1)
                    {
                        column.Values.reserve(definition.arrLength);
                        for(int i = 0; i < definition.arrLength; i++)
                        {
                            auto intValue = bitReader.ReadSignedValue64(32).As<float>();
                            column.Values.push_back(std::to_string(intValue));
                        }
                    }
                	  else
                    {
                        auto intValue = bitReader.ReadSignedValue64(32).As<float>();
                        value = std::to_string(intValue);
                    }
                }
                else
                {
                    assert(false);
                    std::cout << "Unknown Type" << std::endl;
                }


                column.Value = value;
                row.Columns[definition.name] = column;
            }

            return row;
        }

        void WDBCTableReader::CloseAllSections()
        {   
            _recordData.reset();
            _stringTable.clear();
        }

        std::size_t WDBCTableReader::RecordCount()
        {   
            return Header.RecordsCount;
        }

        std::size_t WDBCTableReader::FieldCount()
        {
          return Header.FieldsCount;
        }

        Structures::BlizzardDatabaseRowDefinition WDBCTableReader::RecordDefinition()
        {
            return _versionDefinition.RowDefinition;
        }
    }
}