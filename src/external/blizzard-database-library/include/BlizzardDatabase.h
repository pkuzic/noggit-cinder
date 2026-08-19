#pragma once
#include <string>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <BlizzardDatabaseTable.h>
#include <readers/BlizzardTableReaderFactory.h>
#include <DatabaseDefinition.h>
#include <extensions/StringExtensions.h>
#include <stream/StreamReader.h>
#include <sstream>
#include <functional>

namespace Noggit
{
	class ClientDatabase;
	class ClientDatabaseTable;
}

namespace BlizzardDatabaseLib
{
	class BlizzardDatabase
	{
		friend class BlizzardDatabaseTable;
		friend class Noggit::ClientDatabase;
		friend class Noggit::ClientDatabaseTable;
	private:
		const std::string _databaseDefinitionFilesLocation;
		const Structures::Build _build;
		Reader::BlizzardTableReaderFactory _blizzardTableReaderFactory;

		std::map<std::string, std::shared_ptr<BlizzardDatabaseTable>> _loadedTables;

		std::map<std::string, Structures::VersionDefinition> _table_definitions;
	public:
		BlizzardDatabase(const std::string& databaseDefinitionDirectory, const Structures::Build& build);
	BlizzardDatabaseTable& LoadTable(const std::string& tableName, std::function<std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>(std::string const&)> file_callback);
	
		Structures::VersionDefinition& TableDefinition(const std::string& tableName);
		Structures::BlizzardDatabaseRowDefinition& TableRecordDefinition(const std::string& tableName);
		inline unsigned int getBuild() { return _build.buildId(); };
	protected:
		bool SaveTable(const std::string& outputDirectory, const std::string& tableName, std::vector<Structures::BlizzardDatabaseRow>& rows);
		void UnloadTable(const std::string& tableName);
	};
}