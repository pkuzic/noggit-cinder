#include <readers/BlizzardTableReaderFactory.h>

namespace BlizzardDatabaseLib {
	namespace Reader {	
		std::shared_ptr<IBlizzardTableReader> BlizzardTableReaderFactory::For(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, const std::string& formatSignature)
		{
			std::cout << "File Header Format: " << formatSignature << std::endl;

			if (Extension::String::Compare(formatSignature, std::string("WDC5")))
				return std::make_shared<WDC5TableReader>(streamReader, versionDefinition);
			if (Extension::String::Compare(formatSignature, std::string("WDC3")))
				return std::make_shared<WDC3TableReader>(streamReader, versionDefinition);
			if (Extension::String::Compare(formatSignature, std::string("WDBC")))
				return std::make_shared<WDBCTableReader>(streamReader, versionDefinition);
			return nullptr;
		}
	}
}