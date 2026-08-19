#include "BlizzardDatabase.h"
#include <cassert>

namespace BlizzardDatabaseLib
{
    BlizzardDatabase::BlizzardDatabase(const std::string& databaseDefinitionDirectory, const Structures::Build& build)
    : _databaseDefinitionFilesLocation(databaseDefinitionDirectory)
    , _build(build)
    {
        _loadedTables = std::map<std::string, std::shared_ptr<BlizzardDatabaseTable>>();
        _blizzardTableReaderFactory = Reader::BlizzardTableReaderFactory();
        _table_definitions = std::map<std::string, Structures::VersionDefinition>();
    }

    // must use as a ref, copies not allowed
    BlizzardDatabaseTable& BlizzardDatabase::LoadTable(const std::string& tableName,
       std::function<std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>(std::string const&)> file_callback)
    {
        if (_loadedTables.contains(tableName))
        {
            std::cout << "Table Already Loaded" << std::endl;
            return *_loadedTables[tableName];
        }

        // get table definition
        auto absoluteFilePathOfDatabaseTableDefinition =  std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = databaseDefinition.For(_build, tableDefinition); // unclean table definition.  pruned one from : WDBCTableReader::RecordDefinition()
        tableDefinition.tableName = tableName;

        if (!tableFound)
            std::cout << "Version Not found" << std::endl;

        // HACKFIX START -- We should probably be doing proper detection to see if a .db2 file exists first, if not fallback to .dbc
        auto fileName = "DBFilesClient\\" + tableName + ".dbc";
        const Structures::Build& dbcCutoffBuild = Structures::Build("7.0.3.21287"); // First build with no more DBC files at all.
        if (_build > dbcCutoffBuild) {
            fileName = "DBFilesClient\\" + tableName + ".db2";
        }
        
        auto fileStream = file_callback(fileName);
        // HACKFIX END

        auto streamReader = std::make_shared<Stream::StreamReader>(fileStream);
        auto fileFormatIdentifier = streamReader->ReadString(4);

        auto tableReader = _blizzardTableReaderFactory.For(streamReader, tableDefinition, fileFormatIdentifier);

        auto constructedTable = std::make_shared<BlizzardDatabaseTable>(tableReader, tableName);
        constructedTable->LoadTableStructure();

        _loadedTables.emplace(tableName, constructedTable);

        return *_loadedTables[tableName];
    }

    bool BlizzardDatabase::SaveTable(const std::string& outputDirectory, const std::string& tableName, std::vector<Structures::BlizzardDatabaseRow>& rows)
    {  
        auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
            (tableName + ".dbd");

        auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
        auto tableDefinition = Structures::VersionDefinition();
        auto tableFound = databaseDefinition.For(_build, tableDefinition);

        if (!tableFound)
            std::cout << "Version Not found" << std::endl;

        auto filePath = std::filesystem::path(outputDirectory) / (tableName + ".dbc");
        auto outputStream = std::ofstream(filePath, std::ios::out | std::ios::binary);

        auto fileWriter = Writer::WDBCTableWriter(outputStream,  tableDefinition);

        // TODO : also need to update the table in memory for the change

        return fileWriter.Write(rows);
    }

    void BlizzardDatabase::UnloadTable(const std::string& tableName)
    {
        if (!_loadedTables.contains(tableName))
        {
            std::cout << "Table Not Loaded" << std::endl;
            return;
        }

        auto& table = _loadedTables.at(tableName);

        table.reset();

        _loadedTables.erase(tableName);
    }

    Structures::VersionDefinition& BlizzardDatabase::TableDefinition(const std::string& tableName)
    {
      if (_table_definitions.contains(tableName))
      {
        return _table_definitions[tableName];
      }

      auto absoluteFilePathOfDatabaseTableDefinition = std::filesystem::path(_databaseDefinitionFilesLocation) /
        (tableName + ".dbd");

      auto databaseDefinition = DatabaseDefinition(absoluteFilePathOfDatabaseTableDefinition.generic_string());
      auto tableVersionDefinition = Structures::VersionDefinition();

      auto tableFound = databaseDefinition.For(_build, tableVersionDefinition); // this initializes definition for the version

      if (!tableFound)
      {
        std::cout << "Verion Not found" << std::endl;
        return tableVersionDefinition;
      }

      tableVersionDefinition.tableName = tableName;

      _table_definitions.emplace(tableName, tableVersionDefinition);

      return tableVersionDefinition;
    }

    Structures::BlizzardDatabaseRowDefinition& BlizzardDatabase::TableRecordDefinition(const std::string& tableName)
    {
      return TableDefinition(tableName).RowDefinition;
    }
}
