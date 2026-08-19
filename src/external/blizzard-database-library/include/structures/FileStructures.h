#pragma once
#include <string>
#include <map>
#include <vector>
#include <structures/Build.h>
#include <structures/BuildRange.h>

namespace BlizzardDatabaseLib {
    namespace Structures {

        struct BlizzardDatabaseColumn
        {
            std::string Value;
            std::vector<std::string> Values;
            int ReferenceId;
        };

        struct BlizzardDatabaseRow
        {
            int RecordId = -1;
            std::map<std::string, BlizzardDatabaseColumn> Columns;

            float getFloat(const std::string& field) const { return std::stof(Columns.at(field).Value); };
            unsigned int getUInt(const std::string& field) const { return std::stoi(Columns.at(field).Value); };
            int getInt(const std::string& field) const { return std::stoi(Columns.at(field).Value); };
            std::string getString(const std::string& field) const { return Columns.at(field).Value; };

            std::vector<std::string> getLocalizedString(const std::string& field, int locale = -1) const
              { return Columns.at(field).Values; };


            std::vector<int> getIntArray(const std::string& field) const 
            {
              std::vector<int> result;
              const auto& values = Columns.at(field).Values;
              result.reserve(values.size());
              for (const auto& v : values)
              {
                result.push_back(std::stoi(v));
              }
              return result;
            };

            BlizzardDatabaseRow() = default;
            BlizzardDatabaseRow(int recordId) : RecordId(recordId) {}
        };

        // Definition for BlizzardDatabaseColumn objects
        struct BlizzardDatabaseColumnDefiniton
        {
            std::string Type = "";
            std::string Name = "";
            bool isID;
            int arrLength;
            bool isRelation;
            bool isSigned;
            // int size;
        };

        // Definition for BlizzardDatabaseRow objects
        struct BlizzardDatabaseRowDefinition
        {
          std::vector<BlizzardDatabaseColumnDefiniton> ColumnDefinitions;

          BlizzardDatabaseColumnDefiniton getColumnDefinition(const std::string& columnName)
          {
            for (auto const& column_def : ColumnDefinitions)
            {
              if (column_def.Name == columnName)
              {
                return column_def;
              }
            }
            return BlizzardDatabaseColumnDefiniton();
          }

          std::string getColumnType(const std::string& columnName)
          {
            return getColumnDefinition(columnName).Type;
          }
        };

        struct ColumnDefinition
        {
            std::string type;
            std::string foreignTable;
            std::string foreignColumn;
            bool hasForeignKey;
            bool verified;
            std::string comment;
        };

        struct Definition
        {
            int size; // generated from type, not from the file
            int arrLength;
            std::string name;
            bool isID;
            bool isRelation;
            bool IsInline;
            bool isSigned;
            std::string comment;
        };

        struct VersionDefinitions
        {
            std::vector<Build> builds;
            std::vector<BuildRange> buildRanges;
            std::vector<std::string> layoutHashes;
            std::string comment;
            std::vector<Definition> definitions;

            VersionDefinitions()
            {
                builds = std::vector<Build>();
                buildRanges = std::vector<BuildRange>();
                layoutHashes = std::vector<std::string>();
                comment = std::string();
                definitions = std::vector<Definition>();
            }
        };

        struct DBDefinition
        {
            std::map<std::string, ColumnDefinition> columnDefinitions;
            std::vector<VersionDefinitions> versionDefinitions;

            DBDefinition()
            {
                columnDefinitions = std::map<std::string, ColumnDefinition>();
                versionDefinitions = std::vector<VersionDefinitions>();
            }
        };

        struct VersionDefinition
        {
            friend class DatabaseDefinition;

            std::string tableName;
            Structures::BlizzardDatabaseRowDefinition RowDefinition; // cleaned up data

            // those are initialized by DatabaseDefinition::For
            VersionDefinitions versionDefinitions; // Definitions only column definition for relevent version. internal data
            // std::vector<Definition> versionDefinition;
            std::map<std::string, ColumnDefinition> columnDefinitions; // all columns, even from different versions

            bool hasId;
            int idColumnIndex;

            VersionDefinition()
            {
                // columnDefinitions = std::map<std::string, ColumnDefinition>();
                versionDefinitions = VersionDefinitions();
                // versionDefinition = std::vector<Definition>();

                hasId = false;
                idColumnIndex = 0;
            }

            void initializeRowDefinition()
            {
              auto recordDefinition = Structures::BlizzardDatabaseRowDefinition();
              recordDefinition.ColumnDefinitions.reserve(versionDefinitions.definitions.size());

              for (auto& columnInformation : versionDefinitions.definitions)
              {
                auto column = Structures::BlizzardDatabaseColumnDefiniton();
                column.Type = columnDefinitions[columnInformation.name].type;
                column.Name = columnInformation.name;
                column.arrLength = columnInformation.arrLength;
                column.isID = columnInformation.isID;
                column.isRelation = columnInformation.isRelation;
                column.isSigned = columnInformation.isSigned;

                recordDefinition.ColumnDefinitions.push_back(column);
              }
              RowDefinition = recordDefinition;
            }
        private:


        };
    }
}