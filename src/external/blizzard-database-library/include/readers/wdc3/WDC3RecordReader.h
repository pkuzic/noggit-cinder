#pragma once
#include <vector>
#include <stream/BitReader.h>
#include <stream/StreamReader.h>
#include <structures/FileStructures.h>
#include <extensions/FlagExtensions.h>
#include <cassert>

namespace BlizzardDatabaseLib {
    namespace Reader {

        class WDC3RecordReader
        {        
            Structures::WDC3Header& _fileHeader;
            Stream::BitReader& _bitReader;
            Structures::VersionDefinition& _versionDefinition;

            std::shared_ptr<Stream::StreamReader> _streamReader;
        public:
            WDC3RecordReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, Stream::BitReader& bitReader, Structures::WDC3Header& fileHeader);
           
            Structures::BlizzardDatabaseRow ReadRecord(int indexOfId, Structures::WDC3Section& section, Stream::BitReader& reader,
                std::vector<Structures::FieldMeta>& fieldMetaData, std::vector<Structures::ColumnMetaData>& columnMetaData,std::map<int, std::vector<Structures::Int32>>& palletMetaData,
                std::map<int, std::map<int, Structures::Int32>>& commonMetaData,Structures::ReferenceData& referenceData, std::vector<int>& indexData);
        private:
            template<typename T>
            T GetFieldValue(int Id, Stream::BitReader& reader, Structures::FieldMeta& fieldMeta,
                Structures::ColumnMetaData& columnMeta, std::vector<Structures::Int32>& palletData, std::map<int, Structures::Int32>& commonData)
            {
                switch (columnMeta.Compression)
                {
                case Structures::CompressionType::None:
                {
                    auto bitSize = 32 - fieldMeta.Bits;
                    if (bitSize <= 0)
                        bitSize = columnMeta.compressionData.Immediate.BitWidth;

                    return reader.ReadValue64(bitSize).As<T>();
                }
                case  Structures::CompressionType::SignedImmediate:
                {
                    return reader.ReadSignedValue64(columnMeta.compressionData.Immediate.BitWidth).As<T>();
                }
                case  Structures::CompressionType::Immediate:
                {
                    return reader.ReadValue64(columnMeta.compressionData.Immediate.BitWidth).As<T>();
                }
                case  Structures::CompressionType::Common:
                {
                    if (commonData.contains(Id))
                        return commonData.at(Id).As<T>();

                    return columnMeta.compressionData.Common.DefaultValue.As<T>();
                }
                case  Structures::CompressionType::Pallet:
                {
                    auto value = reader.ReadUint32(columnMeta.compressionData.Pallet.BitWidth);
                    return palletData[value].As<T>();
                }
                case  Structures::CompressionType::PalletArray:
                {
                    if (columnMeta.compressionData.Pallet.Cardinality != 1)
                        break;

                    auto palletArrayIndex = reader.ReadUint32(columnMeta.compressionData.Pallet.BitWidth);
                    return palletData[palletArrayIndex].As<T>();
                }
                default:
                  assert(false);
                }

                return static_cast<T>(0);
            }

            template<typename T>
            std::vector<T> GetFieldArrayValue(int Id, Stream::BitReader& reader, Structures::FieldMeta& fieldMeta,
                Structures::ColumnMetaData& columnMeta, std::vector<Structures::Int32>& palletData, std::map<int, Structures::Int32>& commonData)
            {
                auto vector = std::vector<T>();
                switch (columnMeta.Compression)
                {
                case  Structures::CompressionType::None:
                {
                    auto bitSize = 32 - fieldMeta.Bits;
                    if (bitSize <= 0)
                        bitSize = columnMeta.compressionData.Immediate.BitWidth;

                    auto entires = columnMeta.Size / bitSize;
                    for (auto i = 0; i < entires; i++)
                    {
                        auto entry = reader.ReadValue64(bitSize).As<T>();
                        vector.push_back(entry);
                    }
                    break;
                }
                case  Structures::CompressionType::PalletArray:
                {
                    auto cardinality = columnMeta.compressionData.Pallet.Cardinality;
                    auto index = reader.ReadUint32(columnMeta.compressionData.Pallet.BitWidth);

                    for (auto i = 0; i < cardinality; i++)
                    {
                        auto data = palletData[i + cardinality * (index)].As<T>();
                        vector.push_back(data);
                    }
                    break;
                }
                  default:
                    assert(false);
                }
                return vector;
            }

            std::vector<std::string> GetFieldStringArrayValue(int offset, int startOfStringTable, Stream::BitReader& reader,Structures::FieldMeta& fieldMeta,
                Structures::ColumnMetaData& columnMeta, std::vector<Structures::Int32>& palletData, std::map<int, Structures::Int32>& commonData)
            {
                auto vector = std::vector<std::string>();
                switch (columnMeta.Compression)
                {
                case Structures::CompressionType::None:
                {
                    auto bitSize = 32 - fieldMeta.Bits;
                    if (bitSize <= 0)
                        bitSize = columnMeta.compressionData.Immediate.BitWidth;

                    auto entires = columnMeta.Size / bitSize;
                    for (auto i = 0; i < entires; i++)
                    {
                        auto entryIndex = (reader.Position >> 3) + offset + reader.ReadValue64(bitSize).As<int>();

                        _streamReader->Jump(startOfStringTable + entryIndex);
                        auto value = _streamReader->ReadString();

                        vector.push_back(value);
                    }
                }
                default:
                  assert(false);
                }

                return vector;
            }
        };
    }
}