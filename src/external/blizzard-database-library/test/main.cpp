#include <BlizzardDatabase.h>
#include <iostream>
#include <string>
#include <BlizzardDatabaseTable.h>
#include <filesystem>
#include <chrono>
#include <thread>

int main(int argc, char* argv[])
{
    std::string dbcFileDirectory = std::string();
    std::string dbcWrathFileDirectory = std::string();
    std::string dbdFileDirectory = std::string();

    for(auto index =0; index < argc; index++)
    {
        auto argument = std::string(argv[index]);
        auto path = std::string("-DBP");
        auto match = strncmp(argument.c_str(), path.c_str(), path.size());

        if(match == 0) //match found
        {
            dbcFileDirectory =  argument.substr(5, argument.size() - 5);
            std::cout << "Database Directory Found: " << dbcFileDirectory << std::endl;
        }

        argument = std::string(argv[index]);
        path = std::string("-DDP");
        match = strncmp(argument.c_str(), path.c_str(), path.size());

        if (match == 0) //match found
        {
            dbdFileDirectory = argument.substr(5, argument.size() - 5);
            std::cout << "Database Definitions Directory Found: " << dbdFileDirectory << std::endl;
        }

        argument = std::string(argv[index]);
        path = std::string("-DWBP");
        match = strncmp(argument.c_str(), path.c_str(), path.size());

        if (match == 0) //match found
        {
            dbcWrathFileDirectory = argument.substr(6, argument.size() - 6);
            std::cout << "Database Directory Wrath Found: " << dbcWrathFileDirectory << std::endl;
        }
    }

    if(dbcFileDirectory.empty())
    {
        std::cout << "No Database Directory Found" << std::endl;
        std::cout << "Expected Argument -DBP:<Path>" << std::endl;
    }

    if (dbdFileDirectory.empty())
    {
        std::cout << "No Defintion Directory Found" << std::endl;
        std::cout << "Expected Argument -DDP:<Path>" << std::endl;
    }

    const auto& build = BlizzardDatabaseLib::Structures::Build("9.1.0.39584");
    const auto& build_wrath = BlizzardDatabaseLib::Structures::Build("3.3.5.12340");
    const auto& table = std::string("map");
    const auto& areaTableName = std::string("areaTable");
    const auto& sparseTable = std::string("ItemSparse");

    auto blizzardDatabase = BlizzardDatabaseLib::BlizzardDatabase(dbdFileDirectory, build_wrath);
   // auto mapTable = blizzardDatabase.LoadTable(table);
   // auto areaTable = blizzardDatabase.LoadTable(areaTableName);

   //auto iterator = mapTable.Records();
   //while (iterator.HasRecords())
   //{
   //    auto record = iterator.Next();
   //
   //    if(!record.Columns.empty())
   //        std::cout << record.RecordId << "=>" << record.Columns.at("MapName_lang").Value << std::endl;
   //}
   //
   //auto firstEntry = mapTable.Record(0); //Azeroth
   //auto moltenCore = mapTable.Record(409); //MoltenCore
   //auto LastEntry = mapTable.Record(724); //Azeroth

   //auto blizzardDatabase = BlizzardDatabaseLib::BlizzardDatabase(dbcFileDirectory, dbdFileDirectory);
   //auto mapTable = blizzardDatabase.LoadTable(table, build);

   //auto DG = mapTable.Record(2245); //Deepwind Gorge
   //auto AV = mapTable.Record(30); //Alterac Valley
   //auto AC = mapTable.Record(37); //Azshara Crater
   //
   //auto iterator = mapTable.Records();
   //while (iterator.HasRecords())
   //{
   //    auto record = iterator.Next();
   //
   //    if(!record.Columns.empty())
   //        std::cout << record.RecordId << "=>" << record.Columns.at("MapName_lang").Value << std::endl;
   //}
   //
   //auto newRecordDefinition = mapTable.GetRecordDefinition();
   //newRecordDefinition = AV;
   //
   //auto newColumn = BlizzardDatabaseLib::Structures::BlizzardDatabaseColumn();
   //newColumn.Value = std::string("Custom AV");
   //newRecordDefinition.Columns["MapName_lang"] = newColumn;
   //
   //mapTable.WriterRecord(newRecordDefinition);
   //
   //AC = mapTable.Record(37); //Azshara Crater
   //
   //blizzardDatabase.UnloadTable(table);
   //
   //auto itemTable = blizzardDatabase.LoadTable(sparseTable, build);
   //auto item = itemTable.Record(187111); //Memory of Blind Faith
   //blizzardDatabase.UnloadTable(sparseTable);

    //auto achievementTable = blizzardDatabase.LoadTable("achievement");
    //auto areapoiTable = blizzardDatabase.LoadTable("areapoi");
    //auto BattlePetEffectPropertiesTable = blizzardDatabase.LoadTable("BattlePetEffectProperties");
    //auto CharacterLoadoutTable = blizzardDatabase.LoadTable("CharacterLoadout");
    //auto ItemSparseTable = blizzardDatabase.LoadTable("ItemSparse");
    //auto UIDungeonScoreRarityTable = blizzardDatabase.LoadTable("UIDungeonScoreRarity");

    return 0;
}