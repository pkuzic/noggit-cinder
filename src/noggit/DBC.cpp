// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#include <noggit/DBC.h>
#include <noggit/Log.h>
#include <noggit/Misc.h>
#include <noggit/project/CurrentProject.hpp>
#include <blizzard-archive-library/include/ClientData.hpp>
#include <string>
#include <Exception.hpp>

AreaDB gAreaDB;
AreaTriggerDB gAreaTriggerDB;
MapDB gMapDB;
LoadingScreensDB gLoadingScreensDB;
LightDB gLightDB;
LightParamsDB gLightParamsDB;
LightSkyboxDB gLightSkyboxDB;
LightIntBandDB gLightIntBandDB;
LightFloatBandDB gLightFloatBandDB;
GroundEffectDoodadDB gGroundEffectDoodadDB;
GroundEffectTextureDB gGroundEffectTextureDB;
TerrainTypeDB gTerrainTypeDB;
LiquidTypeDB gLiquidTypeDB;
SoundProviderPreferencesDB gSoundProviderPreferencesDB;
SoundAmbienceDB gSoundAmbienceDB;
ZoneMusicDB gZoneMusicDB;
ZoneIntroMusicTableDB gZoneIntroMusicTableDB;
SoundEntriesDB gSoundEntriesDB;
WMOAreaTableDB gWMOAreaTableDB;

void OpenDBs(std::shared_ptr<BlizzardArchive::ClientData> clientData)
{
  Log << "Opening client DBCs..." << std::endl;

  bool const is_vanilla =
    Noggit::Project::CurrentProject::get()->projectVersion == Noggit::Project::ProjectVersion::VANILLA;

  // Vanilla localized strings have 8 locale columns; TBC+ have 16. This drives
  // DBCFile::getLocalizedString and every hardcoded post-name column index.
  DBCFile::LocaleCount = is_vanilla ? 8 : 16;

  // Open each DBC independently so one missing/corrupt file can't skip the rest, and skip
  // the tables that simply do not exist pre-TBC (LiquidType/LightParams/LightSkybox/
  // WMOAreaTable). Their accessors already degrade gracefully on an empty table.
  auto open_db = [&clientData](DBCFile& db, const char* name, bool present_in_version = true)
  {
    if (!present_in_version)
    {
      Log << "Skipping " << name << " (not present in this client version)" << std::endl;
      return;
    }
    try
    {
      db.open(clientData);
    }
    catch (std::exception const& e)
    {
      LogError << "Failed to open " << name << " : " << e.what() << std::endl;
    }
    catch (...)
    {
      LogError << "Failed to open " << name << " : unhandled exception" << std::endl;
    }
  };

  open_db(gAreaDB, "AreaTable.dbc");
  open_db(gAreaTriggerDB, "AreaTrigger.dbc");
  open_db(gMapDB, "Map.dbc");
  open_db(gLoadingScreensDB, "LoadingScreens.dbc");
  open_db(gLightDB, "Light.dbc");
  open_db(gLightParamsDB, "LightParams.dbc", !is_vanilla); // TBC+
  open_db(gLightSkyboxDB, "LightSkybox.dbc", !is_vanilla); // TBC+
  open_db(gLightIntBandDB, "LightIntBand.dbc");
  open_db(gLightFloatBandDB, "LightFloatBand.dbc");
  open_db(gGroundEffectDoodadDB, "GroundEffectDoodad.dbc");
  open_db(gGroundEffectTextureDB, "GroundEffectTexture.dbc");
  open_db(gTerrainTypeDB, "TerrainType.dbc");
  open_db(gLiquidTypeDB, "LiquidType.dbc", !is_vanilla); // TBC+
  open_db(gSoundProviderPreferencesDB, "SoundProviderPreferences.dbc");
  open_db(gSoundAmbienceDB, "SoundAmbience.dbc");
  open_db(gZoneMusicDB, "ZoneMusic.dbc");
  open_db(gZoneIntroMusicTableDB, "ZoneIntroMusicTable.dbc");
  open_db(gSoundEntriesDB, "SoundEntries.dbc");
  open_db(gWMOAreaTableDB, "WMOAreaTable.dbc", !is_vanilla); // TBC+
}


// includes the parent zone name as a prefix
std::string AreaDB::getAreaFullName(int pAreaID)
{
  if (!pAreaID || pAreaID == -1)
  {
    return "Unknown location";
  }    

  unsigned int regionID = 0;
  std::string areaName = "";
  try
  {
    AreaDB::Record rec = gAreaDB.getByID(pAreaID);
    areaName = rec.getLocalizedString(AreaDB::Name);
    regionID = rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    areaName = "Unknown location";
  }
  if (regionID != 0)
  {
    try
    {
      AreaDB::Record rec = gAreaDB.getByID(regionID);
      areaName = std::string(rec.getLocalizedString(AreaDB::Name)) + std::string(": ") + areaName;
    }
    catch (AreaDB::NotFound)
    {
      areaName = "Unknown location";
    }
  }

  return areaName;
}

std::uint32_t AreaDB::get_area_parent(int area_id)
{
  // todo: differentiate between no parent and error ?
  if (!area_id || area_id == -1)
  {
    return 0;
  }

  try
  {
    AreaDB::Record rec = gAreaDB.getByID(area_id);
    return rec.getUInt(AreaDB::Region);
  }
  catch (AreaDB::NotFound)
  {
    return 0;
  }
}

std::uint32_t AreaDB::get_new_areabit()
{
    unsigned int areabit = 0;

    for (Iterator i = gAreaDB.begin(); i != gAreaDB.end(); ++i)
    {
        areabit = std::max(i->getUInt(AreaDB::AreaBit), areabit);
    }

    return static_cast<int>(++areabit);
}

std::string MapDB::getMapName(int pMapID)
{
  if (pMapID<0) return "Unknown map";
  std::string mapName = "";
  try
  {
    MapDB::Record rec = gMapDB.getByID(pMapID);
    // Vanilla Map.dbc layout (verified against a real 1.12 client): id(0), Directory(1),
    // InstanceType(2), Name(3, 8-locale block), nameFlags(11)... — so the localized name is
    // at field 3, not WotLK's 5. Fall back to the internal directory name if it's empty.
    bool const is_vanilla =
      Noggit::Project::CurrentProject::get()->projectVersion == Noggit::Project::ProjectVersion::VANILLA;
    size_t const name_field = is_vanilla ? 3 : MapDB::Name;
    mapName = std::string(rec.getLocalizedString(name_field));
    if (mapName.empty())
      mapName = std::string(rec.getString(MapDB::InternalName));
  }
  catch (MapDB::NotFound)
  {
    mapName = "Unknown map";
  }

  return mapName;
}

int MapDB::findMapName(const std::string &map_name)
{
  for (Iterator i = gMapDB.begin(); i != gMapDB.end(); ++i)
  {
    if (i->getString(MapDB::InternalName) == map_name)
    {
      return static_cast<int>(i->getUInt(MapDB::MapID));
    }
  }

  return -1;
}

const char * getGroundEffectDoodad(unsigned int effectID, int DoodadNum)
{
  try
  {
    unsigned int doodadId = gGroundEffectTextureDB.getByID(effectID).getUInt(GroundEffectTextureDB::Doodads + DoodadNum);
    return gGroundEffectDoodadDB.getByID(doodadId).getString(GroundEffectDoodadDB::Filename);
  }
  catch (DBCFile::NotFound)
  {
    LogError << "Tried to get a not existing row in GroundEffectTextureDB or GroundEffectDoodadDB ( effectID = " << effectID << ", DoodadNum = " << DoodadNum << " )!" << std::endl;
    return 0;
  }
}

int LiquidTypeDB::getLiquidType(int pID)
{
  int type = 0;
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    type = rec.getUInt(LiquidTypeDB::Type);
  }
  catch (LiquidTypeDB::NotFound)
  {
    type = 0;
  }
  return type;
}

std::string  LiquidTypeDB::getLiquidName(int pID)
{
  std::string type = "";
  try
  {
    LiquidTypeDB::Record rec = gLiquidTypeDB.getByID(pID);
    type = std::string(rec.getString(LiquidTypeDB::Name));
  }
  catch (MapDB::NotFound)
  {
    type = "Unknown type";
  }

  return type;
}

std::string WMOAreaTableDB::getWMOAreaName(int WMOId, int namesetId)
{
    if (WMOId == -1)
    {
        return "Unknown location";
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getUInt(WMOAreaTableDB::NameSetId) == namesetId && i->getInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            // wmoareatableid = i->getUInt(WMOAreaTableDB::ID);
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                return areaName;
            else
            {   // get name from area instead
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    // return AreaDB::getAreaFullName(areatableid); // full name with zone
                    std::string arena_name = "";
                    try
                    {
                        auto rec = gAreaDB.getByID(areatableid);
                        arena_name =  rec.getLocalizedString(AreaDB::Name);
                    }
                    catch (WMOAreaTableDB::NotFound)
                    {
                        arena_name = "Unknown location";
                    }
                    return areaName;
                }
                else
                {
                    // if no areaId is set in the WMOAreaTableDB record, client uses the local terrain area id.
                    return "-Local Terrain Area-";
                }

            }
        }
    }
    throw NotFound();
}

std::vector<std::string> WMOAreaTableDB::getWMOAreaNames(int WMOId)
{
    std::vector<std::string> areanamesvect;

    if (WMOId == -1)
    {
        return areanamesvect;
    }

    for (Iterator i = gWMOAreaTableDB.begin(); i != gWMOAreaTableDB.end(); ++i)
    {
        if (i->getUInt(WMOAreaTableDB::WmoId) == WMOId && i->getInt(WMOAreaTableDB::WMOGroupID) == -1)
        {
            // wmoareatableid = i->getUInt(WMOAreaTableDB::ID);
            std::string areaName = i->getLocalizedString(WMOAreaTableDB::Name);

            if (!areaName.empty())
                areanamesvect.push_back(areaName);
            else
            {   // get name from area instead
                int areatableid = i->getUInt(WMOAreaTableDB::AreaTableRefId);
                if (areatableid)
                {
                    try
                    {
                        auto rec = gAreaDB.getByID(areatableid);
                        areanamesvect.push_back(rec.getLocalizedString(AreaDB::Name));
                    }
                    catch (WMOAreaTableDB::NotFound)
                    {
                        areanamesvect.push_back("Unknown location");
                    }

                }
                else
                    areanamesvect.push_back("-Local Terrain Area-"); // nullptr? need to get it from terrain
            }
        }
        // could optimise and break when iterator WmoId is higher than the Wmodid, but this wouldn't support unordered DBCs. Client does this.
    }
    return areanamesvect;
}
