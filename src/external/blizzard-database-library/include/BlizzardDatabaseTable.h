#pragma once
#include <vector>
#include <BlizzardDatabaseRecordCollection.h>
#include <BlizzardDatabaseRecord.h>
#include <readers/IBlizzardTableReader.h>
#include <structures/FileStructures.h>
#include <string>
#include <algorithm>
#include <structures/Build.h>

namespace BlizzardDatabaseLib {

    class BlizzardDatabaseTable
    {
        friend class BlizzardDatabase;
    private:
        std::shared_ptr<Reader::IBlizzardTableReader> _tableReader;

        const std::string _tableName;
    public:
        BlizzardDatabaseTable(std::shared_ptr<Reader::IBlizzardTableReader> tableReader, std::string const& tableName)
            : _tableReader(tableReader), _tableName(tableName)
        {

        }

        ~BlizzardDatabaseTable()
        {
            // TODO : when copied BlizzardDatabaseTable objects go out of scope they call this, and it resets the reader for the main object too!
            _tableReader->CloseAllSections();
        }
        // force no copy for the problem above
        BlizzardDatabaseTable(const BlizzardDatabaseTable&) = delete;
        BlizzardDatabaseTable& operator=(const BlizzardDatabaseTable&) = delete;

        BlizzardDatabaseTable(BlizzardDatabaseTable&&) noexcept = default;
        BlizzardDatabaseTable& operator=(BlizzardDatabaseTable&&) noexcept = default;

        unsigned int RecordCount() const
        {
            return static_cast<unsigned int>(_tableReader->RecordCount());
        }

        // column count from file header, not definition file
        int ColumnCount() const
        {
          return static_cast<int>(_tableReader->FieldCount());
        }

        std::string Name() const
        {
          return _tableName;
        }

        Structures::BlizzardDatabaseRow RecordById(unsigned int id) const
        {
           return _tableReader->RecordById(id);
        }

        Structures::BlizzardDatabaseRow RecordByPosition(unsigned int positionId) const
        {
          return _tableReader->Record(positionId);
        }

        BlizzardDatabaseRecordCollection Records() const
        {
            return BlizzardDatabaseRecordCollection(_tableReader);
        }

        Structures::BlizzardDatabaseRowDefinition GetRecordDefinition() const
        {
            return _tableReader->RecordDefinition();
        }

        void WriteRecord(Structures::BlizzardDatabaseRow& newRecord)
        {


            //Reload data so it contains new record
            _tableReader->CloseAllSections();
            _tableReader->LoadTableStructure();
        }
    private:
        void LoadTableStructure()
        {  
            _tableReader->LoadTableStructure();
        }
    };
}

