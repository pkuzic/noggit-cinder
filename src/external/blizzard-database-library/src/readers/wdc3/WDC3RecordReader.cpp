#include <readers/wdc3/WDC3RecordReader.h>


namespace BlizzardDatabaseLib {
    namespace Reader {
        WDC3RecordReader::WDC3RecordReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, Stream::BitReader& bitReader, Structures::WDC3Header& fileHeader)
            : _bitReader(bitReader), _streamReader(streamReader), _versionDefinition(versionDefinition), _fileHeader(fileHeader)
        {

        }

        Structures::BlizzardDatabaseRow WDC3RecordReader::ReadRecord(int indexOfId, Structures::WDC3Section& section, Stream::BitReader& reader,
            std::vector<Structures::FieldMeta>& fieldMetaData, std::vector<Structures::ColumnMetaData>& columnMetaData, std::map<int, std::vector<Structures::Int32>>& palletMetaData,
            std::map<int, std::map<int, Structures::Int32>>& commonMetaData,Structures::ReferenceData& referenceData, std::vector<int>& indexData)
        {
            auto recordBlockSize = section.NumRecords * _fileHeader.RecordSize;
            auto startOfStringTable = section.FileOffset + (recordBlockSize);

            auto Id = section.IndexDataSize != 0 ? indexData[indexOfId] : -1;
            auto columns = _versionDefinition.columnDefinitions;
            auto versionDefs = _versionDefinition.versionDefinitions;

            Structures::BlizzardDatabaseRow row = Structures::BlizzardDatabaseRow();
            if (Id != -1)
            {
                row = Structures::BlizzardDatabaseRow(Id);
            }

            for (int def = 0; def < columnMetaData.size(); def++)
            {
                auto definitionIndex = Id == -1 ? def : def + 1;
                auto column = versionDefs.definitions[definitionIndex];
                if (!column.IsInline)
                    column = versionDefs.definitions[definitionIndex + 1];

                auto fieldMeta = fieldMetaData.at(def);
                auto columnMeta = columnMetaData.at(def);
                auto commonData = std::map<int, Structures::Int32>();
                auto palletData = std::vector<Structures::Int32>();

                if (commonMetaData.contains(def))
                    commonData = commonMetaData.at(def);

                if (palletMetaData.contains(def))
                    palletData = palletMetaData.at(def);

                auto tablecolumn = columns.at(column.name);
                auto type = tablecolumn.type;
                auto columnValue = std::string();

                Structures::BlizzardDatabaseColumn blizzColumn = Structures::BlizzardDatabaseColumn();
                if (referenceData.Entries.contains(indexOfId))
                {
                    blizzColumn.ReferenceId = referenceData.Entries.at(indexOfId);
                }

                row.Columns.emplace(column.name, blizzColumn);

                if (Extension::String::Compare(type, "int"))
                {
                    if (column.arrLength > 0)
                    {
                        auto value = GetFieldArrayValue<unsigned int>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);

                       //TODO: Handle these Values
                    }

                    long long value = 0;
                    if (column.size == 8 && column.isSigned)
                        value = GetFieldValue<char>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 8 && !column.isSigned)
                        value = GetFieldValue<unsigned char>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 16 && column.isSigned)
                        value = GetFieldValue<short>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 16 && !column.isSigned)
                        value = GetFieldValue<unsigned short>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 32 && column.isSigned)
                        value = GetFieldValue<int>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 32 && !column.isSigned)
                        value = GetFieldValue<unsigned int>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 64 && column.isSigned)
                        value = GetFieldValue<long long>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                    if (column.size == 64 && !column.isSigned)
                        value = GetFieldValue<unsigned long long>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);

                    row.Columns[column.name].Value = std::to_string(value);             
                }

                if (Extension::String::Compare(type, "float"))
                {
                    if (column.arrLength > 0)
                    {
                        auto value = GetFieldArrayValue<float>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                       //TODO: Handle these values
                    }
                    else
                    {
                        auto value = GetFieldValue<float>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                        row.Columns[column.name].Value = std::to_string(value);
                    }
                }

                if (Extension::String::Compare(type, "string") || Extension::String::Compare(type, "locstring"))
                {
                    if (column.arrLength > 0)
                    {
                        auto readerOffset = (indexOfId * _fileHeader.RecordSize) - (_fileHeader.RecordsCount * _fileHeader.RecordSize);
                        auto entries = GetFieldStringArrayValue(readerOffset, startOfStringTable, _bitReader, fieldMeta, columnMeta, palletData, commonData);

                        row.Columns[column.name].Values = entries;
                        //TODO: Handle these values
                    }

                    if (Extension::Flag::HasFlag(_fileHeader.Flags, Flag::DatabaseVersion2Flag::VariableWidthRecord))
                    {
                        auto value = _bitReader.ReadNullTermintingString();
                        row.Columns[column.name].Value = value;
                    }
                    else
                    {
                     
                        auto readerOffset = (indexOfId * _fileHeader.RecordSize) - (_fileHeader.RecordsCount * _fileHeader.RecordSize);
                        auto offsetPosition = readerOffset + (_bitReader.Position >> 3);     
                        auto lookupId = GetFieldValue<int>(Id, _bitReader, fieldMeta, columnMeta, palletData, commonData);
                        auto stringLookupIndex = offsetPosition + (int)lookupId;

                        _streamReader->Jump(startOfStringTable + stringLookupIndex);

                        auto value = _streamReader->ReadString();

                        row.Columns[column.name].Value = value;
                    }
                }
            }

            return row;
        }
    }
}