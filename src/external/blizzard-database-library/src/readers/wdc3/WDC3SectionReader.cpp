#include <readers/wdc3/WDC3SectionReader.h>

namespace BlizzardDatabaseLib {
    namespace Reader {

        WDC3SectionReader::WDC3SectionReader(std::shared_ptr<Stream::StreamReader> _streamReader, Structures::WDC3Section section, Structures::WDC3Header& _header)
            : _streamReader(_streamReader), Section(section), _sectionStartoffset(section.FileOffset), _recordCount(section.NumRecords), _header(_header)
        {

        }

        std::shared_ptr<char[]> WDC3SectionReader::GetSection()
        {
            if (!IsOpen)
                return nullptr;
            return _sectionDataBlock;
        }

        std::shared_ptr<char[]> WDC3SectionReader::OpenSection()
        {
            auto recordBlockSize = _recordCount * _header.RecordSize;
            auto startOfStringTable = _sectionStartoffset + (recordBlockSize);
            auto endOfStringTable = startOfStringTable + Section.StringTableSize;

            _streamReader->Jump(_sectionStartoffset);

            if (Extension::Flag::HasFlag(_header.Flags, Flag::DatabaseVersion2Flag::VariableWidthRecord)) //If the data is Fixed record Width
            {
                recordBlockSize = Section.OffsetRecordsEndOffset - Section.FileOffset;
                _sectionDataBlock = _streamReader->ReadBlock(recordBlockSize);

                if (_streamReader->Position() != Section.OffsetRecordsEndOffset)
                    std::cout << "Over/Under Read Section" << std::endl;
            }
            else //If record data is not fixed width
            {
                _sectionDataBlock = _streamReader->ReadBlock(recordBlockSize);
                _streamReader->Jump(endOfStringTable);
            }

            IndexData = _streamReader->ReadArray<int>(Section.IndexDataSize / 4);
            if (IndexData.size() > 0) {
                //fill index 0-X based on records
            }

            if (Section.CopyTableCount > 0)
            {
                for (int i = 0; i < Section.CopyTableCount; i++)
                {
                    int index = _streamReader->Read<int>();
                    int value = _streamReader->Read<int>();

                    CopyData[index] = value;
                }
            }

            SparseEntryData = std::vector<Structures::SparseEntry>();
            if (Section.OffsetMapIDCount > 0)
            {
                // HACK unittestsparse is malformed and has sparseIndexData first
                if (_header.TableHash == 145293629)
                {
                    auto streamPosition = _streamReader->Position();
                    streamPosition += (4 * Section.OffsetMapIDCount);
                    _streamReader->Jump(streamPosition);
                }

                SparseEntryData = _streamReader->ReadArray<Structures::SparseEntry>(Section.OffsetMapIDCount);
            }

            ReferenceData = Structures::ReferenceData();
            if (Section.ParentLookupDataSize > 0)
            {
                auto oldPosition = _streamReader->Position();
                ReferenceData.NumRecords = _streamReader->Read<unsigned int>();
                ReferenceData.MinId = _streamReader->Read<unsigned int>();
                ReferenceData.MaxId = _streamReader->Read<unsigned int>();

                auto entries = _streamReader->ReadArray<Structures::ReferenceEntry>(ReferenceData.NumRecords);
                for (int i = 0; i < entries.size(); i++)
                {
                    ReferenceData.Entries[entries[i].Index] = entries[i].Id;
                }
            }

            if (Section.OffsetMapIDCount > 0)
            {
                auto sparseIndexData = _streamReader->ReadArray<int>(Section.OffsetMapIDCount);

                if (Section.IndexDataSize > 0 && IndexData.size() != sparseIndexData.size())
                    std::cout << "m_indexData.Length != sparseIndexData.Length" << std::endl;

                IndexData = sparseIndexData;
            }

            IsOpen = true;
            return _sectionDataBlock;
        }
        void WDC3SectionReader::CloseSection()
        {
            std::cout << "Section closed, Record Count : " << Section.NumRecords << std::endl;

            IndexData.clear();
            SparseEntryData.clear();
            CopyData.clear();

            ReferenceData = Structures::ReferenceData();

            IsOpen = false;
            _sectionDataBlock.reset();
        }
    }
}