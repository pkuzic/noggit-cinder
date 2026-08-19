#pragma once
#include <memory>
#include <readers/IBlizzardTableReader.h>
#include <structures/FileStructures.h>

namespace BlizzardDatabaseLib {

    class BlizzardDatabaseRecordCollection
    {
    private:
        int _minIndex = 0;
        int _maxIndex = 0;
        int _currentIndex = -1;
        std::shared_ptr<Reader::IBlizzardTableReader> _tableReader;
        Structures::BlizzardDatabaseRow _currentRecord;
    public:
        BlizzardDatabaseRecordCollection(std::shared_ptr<Reader::IBlizzardTableReader> tableReader)
        {
            // tableReader->CloseAllSections();
            // tableReader->LoadTableStructure();

            _maxIndex = static_cast<int>(tableReader->RecordCount());
            _tableReader = tableReader;
        }

        bool HasRecords()
        {
            return _currentIndex < _maxIndex - 1;
        }

        Structures::BlizzardDatabaseRow& Next()
        {
            _currentIndex++;
            _currentRecord = _tableReader->Record(_currentIndex);
            return _currentRecord;
        }

        // too painful to reimplement in sql and not needed yet
        /*
        Structures::BlizzardDatabaseRow& First()
        {
            _currentRecord = _tableReader->Record(_minIndex);
            return _currentRecord;
        }

        Structures::BlizzardDatabaseRow& Last()
        {
            _currentRecord = _tableReader->Record(_maxIndex);
            return _currentRecord;
        }*/
    };
}