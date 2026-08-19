#pragma once
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <structures/FileStructures.h>
#include <map>
#include <vector>
#include <algorithm>
#include <regex>
#include <extensions/StringExtensions.h>

namespace BlizzardDatabaseLib
{
	class DatabaseDefinition
	{
	private:
		const std::string _databaseDefinitionFile;
	public:
		DatabaseDefinition(const std::string& databaseDefinitionsDirectory);
		Structures::DBDefinition Read();
		bool For(const Structures::Build& build, Structures::VersionDefinition& definition);
	};
}
