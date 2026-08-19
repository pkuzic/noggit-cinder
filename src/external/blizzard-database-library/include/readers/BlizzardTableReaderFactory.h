#pragma once
#include <iostream>
#include <memory>
#include <readers/IBlizzardTableReader.h>
#include <readers/wdc5/WDC5TableReader.h>
#include <readers/wdc3/WDC3TableReader.h>
#include <readers/wdbc/WDBCTableReader.h>
#include <stream/StreamReader.h>

namespace BlizzardDatabaseLib {
	namespace Reader {
		class BlizzardTableReaderFactory
		{
		public:
			std::shared_ptr<IBlizzardTableReader> For(std::shared_ptr<Stream::StreamReader> streamReader, Structures::VersionDefinition& versionDefinition, const std::string& formatSignature);
		};
	}
}