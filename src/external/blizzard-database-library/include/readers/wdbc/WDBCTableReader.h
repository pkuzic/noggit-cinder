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
#include <readers/wdbc/WDBCRecordReader.h>
#include <stream/StreamReader.h>
#include <stream/BitReader.h>
#include <structures/Types.h>
#include <structures/FileStructures.h>
#include <extensions/StringExtensions.h>
#include <extensions/FlagExtensions.h>
#include <extensions/MemoryExtensions.h>
#include <extensions/VectorExtensions.h>
#include <external/ByteStream.h>


namespace BlizzardDatabaseLib {
    namespace Reader {

        class WDBCTableReader : public IBlizzardTableReader
        {       
            std::shared_ptr<Stream::StreamReader> _streamReader;

            Structures::WDBCHeader Header;
            std::shared_ptr<char[]> _recordData;
            std::map<long, std::string> _stringTable;
            Structures::VersionDefinition _versionDefinition; // contains versionDefinitions which has the current version definitions
        public:
            WDBCTableReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition versionDefinition);
            ~WDBCTableReader();
            void LoadTableStructure() override;
            void CloseAllSections() override;
            Structures::BlizzardDatabaseRow RecordById(unsigned int Id) override;
            Structures::BlizzardDatabaseRow Record(unsigned int index) override;
            Structures::BlizzardDatabaseRowDefinition RecordDefinition() override;
            std::size_t RecordCount() override;
            std::size_t FieldCount() override;
        };
    }
    namespace Writer
    {
        class StringTable
        {
            std::map<std::string, int> _stringTable;
            uint32_t index = 1;
        public:
            StringTable()
            {
                _stringTable = std::map<std::string, int>();
            }
            uint32_t Insert(std::string& string)
            {
                if (string == "")
                    return 0;

                if (_stringTable.count(string))
                {
                    return _stringTable[string];
                }

                _stringTable[string] = index;

                index += static_cast<std::uint32_t>(string.length() + 1);
                return  _stringTable[string];
            }

            std::vector<char> ToBuffer()
            {
                auto _indexBasedTable = std::map<int, std::string>();
                for (auto const& entry : _stringTable)
                {
                    _indexBasedTable.emplace(entry.second, entry.first);
                }

                auto stringTablePtr = std::vector<char>();
                stringTablePtr.push_back('\0');
                for (auto const& entry : _indexBasedTable)
                { 
                    auto str = entry.second;
                    std::copy(str.begin(), str.end(), std::back_inserter(stringTablePtr));
                    stringTablePtr.push_back('\0');
                }

                return stringTablePtr;
            }
        };

        template<typename T> inline
        auto write(std::ostream& stream, T const& val) -> void
        {
            stream.write(reinterpret_cast<char const*>(&val), sizeof(T));
        }

        class WDBCTableWriter 
        {
            std::ofstream& _stream;
            Structures::VersionDefinition& _versionDefinition;
        public:
            WDBCTableWriter(std::ofstream& stream,  Structures::VersionDefinition& versionDefinition): _stream(stream) , _versionDefinition(versionDefinition)
            {

            }
            ~WDBCTableWriter()
            {

            }

            bool Write(const std::vector<Structures::BlizzardDatabaseRow>& rows)
            {
                auto recordCount = rows.size();
                auto fieldCount = 0;
                auto recordSize = 0;
                auto stringTableSize = 0;

                //Build Record/Field Size for dbd 
                for (auto const& column : _versionDefinition.versionDefinitions.definitions)
                {
                    auto columnDefintion = _versionDefinition.columnDefinitions[column.name];
                    auto arrayLength = column.arrLength;
                    if (columnDefintion.type == "locstring")
                    {
                        recordSize += 16 * 4;
                        fieldCount += 16;
                    }
                    if (arrayLength > 0)
                    {
                        recordSize += arrayLength * 4;
                        fieldCount += arrayLength;
                    }
                    else
                    {
                        recordSize += 4;
                        fieldCount += 1;
                    }
                }

                ByteStream stream(static_cast<unsigned int>(recordSize * recordCount));
                auto stringTable = StringTable();
                auto stringTableIndex = 1;
                for (auto const& row : rows)
                {
                    auto recordId = row.RecordId;
                    for (auto const& column : _versionDefinition.versionDefinitions.definitions)
                    {
                        auto columnDefintion = _versionDefinition.columnDefinitions[column.name];
                        auto rowColumd = row.Columns.at(column.name);

                        if (columnDefintion.type == "int")
                        {
                            if (column.isID)
                            {
                                stream << (uint32_t)recordId;
                            }
                            else if (column.arrLength > 0)
                            {
                                for (int i = 0; i < column.arrLength; i++)
                                {
                                    auto value = rowColumd.Values[i];
                                    stream << (uint32_t)std::stoi(value);
                                }
                            }
                            else
                            {
                                stream << (uint32_t)std::stoi(rowColumd.Value);
                            }
                        }
                        if (columnDefintion.type == "float")
                        {
                            if (column.arrLength > 0)
                            {
                                for (int i = 0; i < column.arrLength; i++)
                                {
                                    auto value = rowColumd.Values[i];
                                    stream << std::stof(value);
                                }
                            }
                            else
                            {
                                stream << std::stof(rowColumd.Value);
                            }
                        }
                        if (columnDefintion.type == "string")
                        {
                            if (column.arrLength > 0)
                            {
                                for (int i = 0; i < column.arrLength; i++)
                                {
                                    auto value = rowColumd.Values[i];
                                    stream << stringTable.Insert(value);
                                }
                            }
                            else
                            {
                                stream << stringTable.Insert(rowColumd.Value);
                            }
                        }

                        if (columnDefintion.type == "locstring")
                        {
                            for (int i = 0; i < 16; i++)
                            {
                                auto value = rowColumd.Values[i];
                                stream << stringTable.Insert(value);
                            }

                            auto flagValue = row.Columns.at(column.name + "_flags");
                            stream << (uint32_t)std::stoul(flagValue.Value);
                        }
                    }
                }

                auto stringTableBuffer = stringTable.ToBuffer();
                stringTableSize = static_cast<int>(stringTableBuffer.size());

                _stream << 'W' << 'D' << 'B' << 'C';

                write(_stream, (uint32_t)recordCount);
                write(_stream, (uint32_t)fieldCount);
                write(_stream, (uint32_t)recordSize);
                write(_stream, (uint32_t)stringTableSize);

                _stream.write(reinterpret_cast<const char*>(stream.getBuf()), stream.getLength());
                _stream.write(reinterpret_cast<const char*>(stringTableBuffer.data()), stringTableBuffer.size());

                _stream.close();

                return true;
            }
        };
    }
}