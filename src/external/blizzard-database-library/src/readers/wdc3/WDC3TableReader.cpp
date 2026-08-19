#include <readers/wdc3/WDC3TableReader.h>

namespace BlizzardDatabaseLib {
    namespace Reader {
  
        WDC3TableReader::WDC3TableReader(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition versionDefinition) : _streamReader(streamReader), _versionDefinition(versionDefinition)
        {

        }

        WDC3TableReader::~WDC3TableReader()
        {
            _streamReader.reset();
        }


        void WDC3TableReader::LoadTableStructure()
        {
            auto length = _streamReader->Length();
            auto headerSize = sizeof(Structures::WDC3Header);

            if (length < headerSize)
            {
                std::cout << "Error Occured While Parsing WDC3 Header, Header to short." << std::endl;
                return;
            }

            _streamReader->Jump(0);

            auto magicNumber = _streamReader->Read<unsigned int>();
            if (magicNumber != Flag::TableFormatSignatures::WDC3_FMT_SIGNATURE)
            {
                std::cout << "Error Occured While Parsing WDC3 Header, Format Signature doesnt Match." << std::endl;
                return;
            }

            Header = _streamReader->Read<Structures::WDC3Header>();

            if (Header.sectionsCount == 0 || Header.RecordsCount == 0)
                return;

            Sections = _streamReader->ReadArray<Structures::WDC3Section>(Header.sectionsCount);

            auto recordBounds = 0;
            for (auto& section : Sections)
            {
                recordBounds += section.NumRecords;
                auto sectionReader = std::make_shared<WDC3SectionReader>(_streamReader, section, Header);

                if(section.TactKeyLookup == 0)
                    _sectionLookup.emplace(recordBounds, sectionReader);
            }

            Meta = _streamReader->ReadArray<Structures::FieldMeta>(Header.FieldsCount);
            ColumnMeta = _streamReader->ReadArray<Structures::ColumnMetaData>(Header.FieldsCount);

            PalletData = std::map<int, std::vector<Structures::Int32>>();
            for (int i = 0; i < ColumnMeta.size(); i++)
            {
                if (ColumnMeta[i].Compression == Structures::CompressionType::Pallet || ColumnMeta[i].Compression == Structures::CompressionType::PalletArray)
                {
                    auto length = ColumnMeta[i].AdditionalDataSize / sizeof(int);
                    auto pallet = _streamReader->ReadArray<Structures::Int32>(length);
                    PalletData.emplace(i, pallet);
                }
            }

            //-- not yet optimised --
            CommonData = std::map<int, std::map<int, Structures::Int32>>();
            for (int i = 0; i < CommonData.size(); i++)
            {
                if (ColumnMeta[i].Compression == Structures::CompressionType::Common)
                {
                    CommonData[i] = std::map<int, Structures::Int32>();
                    auto entires = ColumnMeta[i].AdditionalDataSize / 8;
                    for (int j = 0; j < static_cast<int>(entires); j++)
                    {
                        auto startOfList = reinterpret_cast<char*>(&CommonData[i][j]);
                        //_streamReader.ReadBlock(startOfList, length * sizeof(int));
                    }
                }
            }
        }

        Structures::BlizzardDatabaseRow WDC3TableReader::RecordById(unsigned int Id) 
        {
            std::shared_ptr<char[]> sectionDataBlock;
            std::unique_ptr<char[]> recordDataBlock;
            std::shared_ptr<WDC3SectionReader> sectionReader;
            for (auto& section : _sectionLookup)
            {        
                sectionReader = section.second;
                sectionDataBlock = sectionReader->OpenSection();

                auto indexOfId = 0U;
                if (!Extension::Vector::IndexOf<int>(sectionReader->IndexData, Id, indexOfId))
                     continue;

                if (Extension::Flag::HasFlag(Header.Flags, Flag::DatabaseVersion2Flag::VariableWidthRecord))
                {
                    auto recordBaseOffset = sectionReader->SparseEntryData[indexOfId].Offset - sectionReader->Section.FileOffset;
                    //auto recordBaseOffset = currentSectionIndex * Header.RecordSize;
                    auto recordLength = sectionReader->SparseEntryData[indexOfId].Size;
                    auto recordStartPtr = sectionDataBlock.get() + recordBaseOffset;
                    recordDataBlock = std::make_unique<char[]>(recordLength);
                
                    //Should be changed to read directly from the section Ptr
                    memcpy(recordDataBlock.get(), recordStartPtr, recordLength);
                }
                else
                {
                    auto recordBaseOffset = indexOfId * Header.RecordSize;
                    auto recordLength = Header.RecordSize;
                    auto recordStartPtr = sectionDataBlock.get() + recordBaseOffset;
                    recordDataBlock = std::make_unique<char[]>(Header.RecordSize);
                
                    //Should be changed to read directly from the section Ptr
                    memcpy(recordDataBlock.get(), recordStartPtr, Header.RecordSize);
                }
                
                auto recordSize = Header.RecordSize;
                auto bitReader = Stream::BitReader(recordDataBlock, Header.RecordSize);
                auto recordReader = WDC3RecordReader(_streamReader, _versionDefinition, bitReader, Header);
                
                auto record = recordReader.ReadRecord(indexOfId, sectionReader->Section, bitReader, Meta, ColumnMeta, PalletData, CommonData, sectionReader->ReferenceData, sectionReader->IndexData);

                return record;
            }
        }

        Structures::BlizzardDatabaseRow WDC3TableReader::Record(unsigned int index)
        {
            std::shared_ptr<char[]> sectionDataBlock;
            std::unique_ptr<char[]> recordDataBlock;
            std::shared_ptr<WDC3SectionReader> sectionReader;
            auto sectionMaxIndex = 0;
            auto currentSectionIndex = 0;
            for (auto& section : _sectionLookup)
            {
                //In this section
                if (static_cast<int>(index) < section.first)
                {
                    sectionReader = section.second;

                    if (!section.second->IsOpen)
                        sectionDataBlock = section.second->OpenSection();       
                    sectionDataBlock = section.second->GetSection();
              
                    currentSectionIndex = index - _sectionMaxIndexCounter;
                    sectionMaxIndex = section.first;

                    break;
                }
            }
            
            if (Extension::Flag::HasFlag(Header.Flags, Flag::DatabaseVersion2Flag::VariableWidthRecord))
            {
               auto recordBaseOffset = sectionReader->SparseEntryData[currentSectionIndex].Offset - sectionReader->Section.FileOffset;
               //auto recordBaseOffset = currentSectionIndex * Header.RecordSize;
               auto recordLength = sectionReader->SparseEntryData[currentSectionIndex].Size;
               auto recordStartPtr = sectionDataBlock.get() + recordBaseOffset;
               recordDataBlock = std::make_unique<char[]>(recordLength);

               //Should be changed to read directly from the section Ptr
               memcpy(recordDataBlock.get(), recordStartPtr, recordLength);
            }
            else
            {
                auto recordBaseOffset = currentSectionIndex * Header.RecordSize;
                auto recordLength = Header.RecordSize;
                auto recordStartPtr = sectionDataBlock.get() + recordBaseOffset;
                recordDataBlock = std::make_unique<char[]>(Header.RecordSize);

                //Should be changed to read directly from the section Ptr
                memcpy(recordDataBlock.get(), recordStartPtr, Header.RecordSize);   
            }

            auto recordSize = Header.RecordSize;
            auto bitReader = Stream::BitReader(recordDataBlock, Header.RecordSize);
            auto recordReader = WDC3RecordReader(_streamReader, _versionDefinition, bitReader, Header);

            auto record =  recordReader.ReadRecord(currentSectionIndex, sectionReader->Section, bitReader, Meta, ColumnMeta, PalletData, CommonData, sectionReader->ReferenceData, sectionReader->IndexData);
          
            if (sectionReader->IsOpen && static_cast<int>(index + 1) >= sectionMaxIndex)
            {
                sectionReader->CloseSection();
                _sectionMaxIndexCounter += currentSectionIndex + 1;
            }

            return record;
        }

        void WDC3TableReader::CloseAllSections()
        {
            for (auto& section : _sectionLookup)
            {
                if (section.second->IsOpen)
                    section.second->CloseSection();
            }
        }

        std::size_t WDC3TableReader::RecordCount()
        {   
            auto recordCount = 0;
            for (auto& section : _sectionLookup)
            {
                recordCount = section.first;           
            }

            return recordCount;
        }

        std::size_t WDC3TableReader::FieldCount()
        {
          return Header.FieldsCount;
        }

        Structures::BlizzardDatabaseRowDefinition WDC3TableReader::RecordDefinition()
        {
            return _versionDefinition.RowDefinition;
        }
    }
}