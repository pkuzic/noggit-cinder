//Folder to contain all of the project related files
#pragma once

#include <memory>
// #include <noggit/application/NoggitApplication.hpp>
#include <noggit/TextureManager.h>
#include <external/tsl/robin_map.h>

#include <filesystem>
#include <vector>
#include <glm/vec3.hpp>


namespace BlizzardDatabaseLib
{
  class BlizzardDatabase;
}

namespace Noggit::Application
{
  struct NoggitApplicationConfiguration;
}

struct TileIndex;
class World;

namespace Noggit::Project
{
  class ApplicationProjectWriter;

  enum class ProjectVersion
  {
    VANILLA,
    BC,
    WOTLK,
    CATA,
    PANDARIA,
    WOD,
    LEGION,
    BFA,
    SL
  };

  struct ClientVersionFactory
  {
    static ProjectVersion mapToEnumVersion(std::string const& projectVersion);

    static std::string MapToStringVersion(ProjectVersion const& projectVersion);
  };

  struct NoggitProjectBookmarkMap
  {
    int map_id;
    std::string name;
    glm::vec3 position;
    float camera_yaw;
    float camera_pitch;
  };

  struct NoggitProjectPinnedMap
  {
    int MapId;
    std::string MapName;
  };

  struct NoggitExtraMapData
  {
  public:
      //Mists Heightmapping
      // Valid for every map that doesn't override its specific data
      tsl::robin_map< std::string, texture_heightmapping_data > TextureHeightData_Global;
      // MapID,TileX,TileY, SMTextureParams for this specific ADT, fallback to global otherwise.
      tsl::robin_map<int, 
            tsl::robin_map< int, tsl::robin_map<int , tsl::robin_map<std::string, texture_heightmapping_data> >>> TextureHeightData_ADT;
      
      void SetTextureHeightData_Global(const std::string& texture, texture_heightmapping_data data, World* worldToUpdate = nullptr); 
      void SetTextureHeightDataForADT(int mapID, const TileIndex& ti, const std::string& texture, texture_heightmapping_data data, World* worldToUpdate = nullptr);

      const texture_heightmapping_data GetTextureHeightDataForADT(int mapID, const TileIndex& ti, const std::string& texture) const;
  };
  
  struct NoggitProjectObjectPalette
  {
      int MapId;
      std::vector<std::string> Filepaths;
  };

  struct NoggitProjectTexturePalette
  {
      int MapId;
      std::vector<std::string> Filepaths;
  };

  struct NoggitProjectSelectionGroups
  {
      int MapId;
      // Might let the user name them later if they get some list UI
      std::vector<std::vector<unsigned int>> SelectionGroups;
  };

  // Record of an MPQ patch this project exported into the client, so a later export
  // can recognise its own patch (safe to replace) versus one changed outside Noggit.
  struct NoggitProjectPatchRecord
  {
      std::string Name;        // archive file name, lower-case (e.g. "patch-z.mpq")
      std::string Md5;         // MD5 hex of the built .mpq as it was written to disk
      long long Size = 0;      // size in bytes of the built .mpq
      std::string BuiltAt;     // human-readable local timestamp of the last build
  };

  class NoggitProject
  {
    std::shared_ptr<ApplicationProjectWriter> _projectWriter;
  public:
    std::string ProjectPath;
    std::string ProjectName;
    std::string ClientPath;
    ProjectVersion projectVersion;
    std::vector<NoggitProjectPinnedMap> PinnedMaps;
    std::vector<NoggitProjectBookmarkMap> Bookmarks;
    std::shared_ptr<BlizzardDatabaseLib::BlizzardDatabase> ClientDatabase;
    std::shared_ptr<BlizzardArchive::ClientData> ClientData;
    std::vector<NoggitProjectObjectPalette> ObjectPalettes;
    std::vector<NoggitProjectTexturePalette> TexturePalettes;
    std::vector<NoggitProjectSelectionGroups> ObjectSelectionGroups;
    std::vector<NoggitProjectPatchRecord> Patches;

    NoggitExtraMapData ExtraMapData;
    NoggitProject();

    unsigned int buildId();

    void createBookmark(const NoggitProjectBookmarkMap& bookmark);

    void deleteBookmark();

    // Upsert (by lower-case Name) a patch record and persist the project. Called after a
    // successful MPQ export so the project remembers the patch it produced.
    void recordPatch(const NoggitProjectPatchRecord& record);

    // Returns the stored record for an archive name (case-insensitive), or nullptr if this
    // project never exported a patch by that name.
    NoggitProjectPatchRecord const* findPatch(const std::string& name) const;

    void pinMap(int map_id, const std::string& map_name);

    void unpinMap(int mapId);

    void saveTexturePalette(const NoggitProjectTexturePalette& new_texture_palette);

    void saveObjectPalette(const NoggitProjectObjectPalette& new_object_palette);

    void saveObjectSelectionGroups(const NoggitProjectSelectionGroups& new_selection_groups);
  };

  class ApplicationProject
  {
    std::shared_ptr<NoggitProject> _active_project;
    std::shared_ptr<Application::NoggitApplicationConfiguration> _configuration;
  public:
    ApplicationProject(std::shared_ptr<Application::NoggitApplicationConfiguration> configuration);

    void createProject(std::filesystem::path const& project_path, std::filesystem::path const& client_path,
                       std::string const& client_version, std::string const& project_name);

    std::shared_ptr<NoggitProject> loadProject(std::filesystem::path const& project_path);

    void loadExtraData(NoggitProject& project);
  };
}
