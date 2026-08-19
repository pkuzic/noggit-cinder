#include <DatabaseDefinition.h>

#include <cassert>

namespace BlizzardDatabaseLib
{

    DatabaseDefinition::DatabaseDefinition(const std::string& databaseDefinitionsFile) : _databaseDefinitionFile(databaseDefinitionsFile)
    {

    }

    Structures::DBDefinition DatabaseDefinition::Read()
    {
        std::ifstream fileStream;
        fileStream.open(_databaseDefinitionFile);

        auto validTypes = std::vector<std::string>{ "uint", "int", "float", "string", "locstring" };
        auto columnDefinitionDictionary = std::map<std::string, Structures::ColumnDefinition>();
        auto lines = std::vector<std::string>();
        auto line = std::string();

        while (std::getline(fileStream, line))
        {
            lines.push_back(line);
        }

        auto lineNumber = 1;
        auto totalLines = lines.size();
        auto firstLine = lines[0];

        if (firstLine.starts_with("COLUMNS") == false)
        {
            std::cout << "unexpected format" << std::endl;
            return Structures::DBDefinition();
        }

        while (lineNumber < totalLines)
        {
            auto line = lines[lineNumber];

            if (line.empty())
                break;

            auto columnDefinition = Structures::ColumnDefinition();

            if (line.find_first_not_of(" ") == std::string::npos)
            {
                std::cout << "format invalid, no type specified" << std::endl;
                return Structures::DBDefinition();
            }

            auto indexOfCommentsStart = line.find_first_of("//");

            auto indexOfForeignKeyStart = line.find_first_of("<");
            if (indexOfForeignKeyStart > indexOfCommentsStart)
                indexOfForeignKeyStart = std::string::npos; // No foreign key if it is after the comment

            auto indexOfForeignKeyEnd = line.find_first_of(">");
            auto indexOfNameStart = line.find_first_of(" ");
            auto indexOfNameEnd = line.find(" ", indexOfNameStart + 1);
            auto indexofTypeEnd = indexOfForeignKeyStart == std::string::npos ? indexOfNameStart : indexOfForeignKeyStart;
            auto columnType = line.substr(0, indexofTypeEnd);

            if (!std::any_of(validTypes.begin(), validTypes.end(), [columnType](std::string comparison) {return comparison == columnType; }))
            {
                std::cout << "column type invalid : " << columnType << std::endl;
                return Structures::DBDefinition();
            }

            columnDefinition.type = columnType;

            if (indexOfForeignKeyStart != std::string::npos && indexOfForeignKeyEnd != std::string::npos)
            {
                auto startIndex = indexOfForeignKeyStart + 1;
                auto foreignKey = line.substr(startIndex, indexOfForeignKeyEnd - startIndex);
                auto foreignKeyComponents = Extension::String::Split(foreignKey, "::");

                if (foreignKeyComponents.size() != 2)
                {
                    std::cout << "Foreign Key Malformed" << std::endl;
                    return Structures::DBDefinition();
                }

                columnDefinition.foreignTable = foreignKeyComponents[0];
                columnDefinition.foreignColumn = foreignKeyComponents[1];
                columnDefinition.hasForeignKey = true;
            }

            auto name = std::string("Nameless Column > ").append(std::to_string(lineNumber));

            if (indexOfNameEnd == std::string::npos)
            {
                name = line.substr(indexOfNameStart + 1);
            }

            if (indexOfNameStart != std::string::npos && indexOfNameEnd != std::string::npos)
            {
                auto adjustedIndexOfNameStart = indexOfNameStart + 1;
                name = line.substr(adjustedIndexOfNameStart, indexOfNameEnd - adjustedIndexOfNameStart);
            }

            if (name.ends_with("?"))
            {
                name = name.substr(0, name.size() - 1);
                columnDefinition.verified = true;
            }

            if (indexOfCommentsStart != std::string::npos)
            {
                columnDefinition.comment = line.substr(indexOfCommentsStart, line.size() - indexOfCommentsStart);
            }

            columnDefinitionDictionary.emplace(name, columnDefinition);

            lineNumber++;
        }

        auto versionDefinitions = std::vector<Structures::VersionDefinitions>();
        auto definitions = std::vector<Structures::Definition>();
        auto layoutHashes = std::vector<std::string>();
        auto builds = std::vector<Structures::Build>();
        auto buildRanges = std::vector<Structures::BuildRange>();
        auto comment = std::string();
        auto startIndex = lineNumber + 1;

        auto const LAYOUT_TOKEN = std::string("LAYOUT");
        auto const BUILD_TOKEN = std::string("BUILD");
        auto const COMMENT_TOKEN = std::string("COMMENT");

        for (auto index = startIndex; index < lines.size();)
        {
            auto line = lines[index];

            if (line.empty())
            {
                auto versionDefinition = Structures::VersionDefinitions();
                versionDefinition.builds = builds;
                versionDefinition.buildRanges = buildRanges;
                versionDefinition.layoutHashes = layoutHashes;
                versionDefinition.definitions = definitions;
                versionDefinition.comment = comment;

                versionDefinitions.push_back(versionDefinition);

                definitions.clear();
                layoutHashes.clear();
                builds.clear();
                buildRanges.clear();
                comment.clear();

                index++;
                continue;
            }

            auto containsLayoutToken = line.starts_with(LAYOUT_TOKEN);
            auto containsBuildToken = line.starts_with(BUILD_TOKEN);
            auto containsCommentToken = line.starts_with(COMMENT_TOKEN);

            if (containsLayoutToken)
            {
                auto layoutTokenSize = LAYOUT_TOKEN.size() - 1;
                auto layoutHashesLine = line.substr(layoutTokenSize);
                layoutHashes = Extension::String::Split(layoutHashesLine, ",");
            }

            if (containsBuildToken)
            {
                auto buildTokenSize = BUILD_TOKEN.size();
                auto buildsLine = line.substr(buildTokenSize);
                auto linkedBuilds = Extension::String::Split(buildsLine, ",");

                for (auto buildString : linkedBuilds)
                {
                    if (buildString.find_first_of('-') != std::string::npos)
                    {
                        auto ranges = Extension::String::Split(buildString, "-");
                        auto minBuild = Structures::Build(ranges[0]);
                        auto maxBuild = Structures::Build(ranges[1]);

                        auto buildRange = Structures::BuildRange(minBuild, maxBuild);
                        buildRanges.push_back(buildRange);
                    }
                    else
                    {
                        auto build = Structures::Build(buildString);
                        builds.push_back(build);
                    }
                }
            }

            if (containsCommentToken)
            {
                auto commentTokenSize = COMMENT_TOKEN.size() - 1;
                comment = line.substr(commentTokenSize);
            }

            if (!containsCommentToken && !containsBuildToken && !containsLayoutToken)
            {
                auto definition = Structures::Definition();
                definition.IsInline = true;

                auto indexOfDollerSignStart = line.find_first_of('$');
                auto indexOfDollerSignEnd = line.find('$', indexOfDollerSignStart + 1);
                if (indexOfDollerSignStart != std::string::npos)
                {
                    auto indexOfFirstDollerSignAdjusted = indexOfDollerSignStart + 1;
                    auto annotations = line.substr(indexOfFirstDollerSignAdjusted, indexOfDollerSignEnd - indexOfFirstDollerSignAdjusted);
                    auto allAnnotations = Extension::String::Split(annotations, ",");

                    for (auto annotation : allAnnotations)
                    {
                        if (Extension::String::Compare(annotation, "id"))
                            definition.isID = true;
                        if (Extension::String::Compare(annotation, "noninline"))
                            definition.IsInline = false;
                        if (Extension::String::Compare(annotation, "relation"))
                            definition.isRelation = true;
                    }

                    line = line.substr(indexOfDollerSignEnd + 1);
                }

                auto indexOfColumnTypeStart = line.find_first_of('<');
                auto indexOfColumnTypeEnd = line.find_first_of('>');
                if (indexOfColumnTypeStart != std::string::npos)
                {
                    auto indexOfColumnTypeStartAdjusted = indexOfColumnTypeStart + 1;
                    auto name = line.substr(0, indexOfColumnTypeStart);
                    auto columnType = line.substr(indexOfColumnTypeStartAdjusted, indexOfColumnTypeEnd - indexOfColumnTypeStartAdjusted);

                    if (columnType[0] == 'u')
                    {
                        columnType = columnType.substr(1);
                        definition.isSigned = false;
                        definition.size = std::atoi(columnType.c_str());
                    }
                    else
                    {
                        definition.isSigned = true;
                        definition.size = std::atoi(columnType.c_str());
                    }

                    definition.name = name;
                }

                auto indexOfEnumLengthStart = line.find_first_of('[');
                auto indexOfEnumLengthEnd = line.find_first_of(']');

                if (indexOfEnumLengthStart != std::string::npos && indexOfEnumLengthEnd != std::string::npos)
                {
                    auto indexOfEnumLengthStartAdjusted = indexOfEnumLengthStart + 1;
                    auto enumLengthString = line.substr(indexOfEnumLengthStartAdjusted, indexOfEnumLengthEnd - indexOfEnumLengthStartAdjusted);

                    definition.arrLength = std::atoi(enumLengthString.c_str());
                }

                auto indexOfCommentStart = line.find_first_of("//");
                if (indexOfCommentStart != std::string::npos)
                {
                    auto indexOfCommentStartAdjusted = indexOfCommentStart + 1;
                    definition.comment = line.substr(indexOfCommentStartAdjusted);
                }

                auto indexOfNameStart = 0;
                auto indexOfNameEnd = line.size();
                if (indexOfCommentStart != std::string::npos)
                    indexOfNameEnd = indexOfCommentStart;
                if (indexOfEnumLengthStart != std::string::npos)
                    indexOfNameEnd = indexOfEnumLengthStart;
                if (indexOfColumnTypeStart != std::string::npos)
                    indexOfNameEnd = indexOfColumnTypeStart;

                definition.name = line.substr(indexOfNameStart, indexOfNameEnd);

                definitions.push_back(definition);

                if (lines.size() == (index + 1))
                {
                    auto versionDefinition = Structures::VersionDefinitions();
                    versionDefinition.builds = builds;
                    versionDefinition.buildRanges = buildRanges;
                    versionDefinition.layoutHashes = layoutHashes;
                    versionDefinition.definitions = definitions;
                    versionDefinition.comment = comment;

                    versionDefinitions.push_back(versionDefinition);
                }
            }

            index++;
        }

        auto databaseDefinition = Structures::DBDefinition();
        databaseDefinition.columnDefinitions = columnDefinitionDictionary;
        databaseDefinition.versionDefinitions = versionDefinitions;

        return databaseDefinition;
    }

    bool DatabaseDefinition::For(const Structures::Build& build, Structures::VersionDefinition& definition)
    {
        auto definitions = Read();

        auto columnDefinitions = definitions.columnDefinitions;
        auto tableVersionDefintions = definitions.versionDefinitions;

        auto versionFound = false;
        definition = Structures::VersionDefinition();
        definition.columnDefinitions = columnDefinitions;

        for (auto& versionDefiniton : tableVersionDefintions)
        {
            auto builds = versionDefiniton.builds;
            if (std::find(builds.begin(), builds.end(), build) != builds.end())
            {
                definition.versionDefinitions = versionDefiniton;
                // definition.versionDefinition = versionDefiniton.definitions;
                versionFound = true;
                break;
            }

            auto buildRanges = versionDefiniton.buildRanges;
            for (auto& buildRange : buildRanges)
            {
                if (buildRange.Contains(build))
                {
                    definition.versionDefinitions = versionDefiniton;
                    // definition.versionDefinition = versionDefiniton.definitions;
                    versionFound = true;
                    break;
                }
            }
            if (versionFound)
              break;
        }
        if (!versionFound)
          return false;

        definition.initializeRowDefinition();
        // set Id column position
        for (int i = 0; i < definition.RowDefinition.ColumnDefinitions.size(); i++)
        {
          auto& internal_definition = definition.versionDefinitions.definitions[i];
          if (internal_definition.isID)
          {
            assert(!definition.hasId); // multiple keys detected
            if (!definition.hasId)
            {
              definition.hasId = true;
              definition.idColumnIndex = i;
            }
          }
        }

        return true;
    }
}