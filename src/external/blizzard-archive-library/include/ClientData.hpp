#ifndef BLIZZARD_ARCHIVE_CLIENT_DATA_HPP
#define BLIZZARD_ARCHIVE_CLIENT_DATA_HPP

#include <array>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <string_view>

#include <Listfile.hpp>
#include <functional>

typedef void* HANDLE;

namespace BlizzardArchive
{

  namespace Archive
  {
    class BaseArchive;
    class MPQArchive;
  }


  enum class ClientVersion : char
  {
    WOTLK = 0,
    SL = 1,
    CLASSIC = 2, // Vanilla 1.12.x, MPQ-based (added for Noggit vanilla support)
  };

  enum class StorageType : char
  {
    MPQ = 0,
    CASC = 1
  };

  enum class OpenMode
  {
    LOCAL,
    REMOTE
  };

  // just used to map to string names in Locales, this isn't the actual locale field in DBCs
  enum class Locale
  {
    AUTO, 
    enGB,
    enUS,
    koKR,
    frFR,
    deDE,
    zhCN,
    enCN,
    zhTW,
    enTW,
    esES,
    esMX,
    ruRU,
    jaJP,
    ptPT,
    ptBR,
    itIT,
    // unknown_12,
    // unknown_13,
    // unknown_14,
    // unknown_15,
  };

  enum class LocaleLang
  {
      enUS = 0,
      enGB = enUS,
      koKR = 1,
      frFR = 2,
      deDE = 3,
      enCN = 4,
      zhCN = enCN,
      enTW = 5,
      zhTW = enTW,
      esES = 6,
      esMX = 7,
      ruRU = 8,
      jaJP = 9, // unused ?
      ptPT = 10,
      ptBR = ptPT,
      itIT = 11,
      /*
      UNK12 = 12,
      UNK13 = 13,
      UNK14 = 14,
      UNK15 = 15,
      */
      NUM_LOCALES = 16 // number of locales
  };

  class ClientData
  {
  public:
    /*
    * path - path to game directory for MPQ-based clients, path to storage directory (the one containing .build.info) for CASC-based clients.
    * CDN URL for online CASC Storages.
    * version - version of the game client. Currently only WotLK and Shadowlands are supported.
    * locale - prefered locale of the client. Wotlk supports automatic detection, for that use Locale::AUTO
    * local_path - project directory, should also contain listfile.csv for CASC-based projects.
    */
    explicit ClientData(std::string const& path
      , ClientVersion version
      , Locale locale
      , std::string const& local_path);

    explicit ClientData(std::string const& path
        , std::string const& cdn_cache_path
        , ClientVersion version
        , Locale locale
        , std::string const& local_path);

    ~ClientData();

    bool mpqArchiveExistsOnDisk(std::string const& archive_name);
    std::optional<Archive::MPQArchive*> getMPQArchive(std::string const& archive_name);
    std::optional<Archive::MPQArchive*> tryCreateMPQArchive(std::string const& archive_name);
    bool isMPQNameValid(std::string const& archive_name, bool exclude_base_mpqs);

    [[nodiscard]]
    ClientVersion version() const { return _version; }

    [[nodiscard]]
    StorageType storageType() const { return _storage_type; }

    [[nodiscard]]
    OpenMode openMode() const { return _open_mode; }

    [[nodiscard]]
    std::string const& path() const { return _path; }
    [[nodiscard]]
    std::string const& projectPath() const { return _local_path; }

    [[nodiscard]]
    BlizzardArchive::Locale const& locale_mode() const { return _locale_mode; }
    std::string_view const locale_name() const { return ClientData::Locales[static_cast<int>(_locale_mode) - 1]; }

    // convert locale name to true locale id for localized strings
    int const getLocaleId() const;

    [[nodiscard]]
    std::string getDiskPath(Listfile::FileKey const& file_key);

    const Listfile::Listfile* listfile() const { return &_listfile; }

    const std::vector<Archive::BaseArchive*>* loadedArchives() const { return &_archives; }

    /* Methods used to universally request client file data in an archive type agnostic way. */

    [[nodiscard]]
    bool readFile(Listfile::FileKey const& file_key, std::vector<char>& buffer);

    // bool addFile(Listfile::FileKey const& file_key, std::vector<char>& buffer, Archive::BaseArchive* dest_archive);
    // bool addFile(Listfile::FileKey const& file_key, Archive::BaseArchive* dest_archive);


    // total files count, files success count
    std::array<int, 2> saveLocalFilesToArchive(Archive::MPQArchive* archive, bool compress, bool compact);

    [[nodiscard]]
    bool exists(Listfile::FileKey const& file_key);

    [[nodiscard]]
    bool existsOnDisk(Listfile::FileKey const& file_key);

    /* Static helper methods */
    [[nodiscard]]
    static std::string normalizeFilenameUnix(std::string filename);

    [[nodiscard]]
    static std::string normalizeFilenameInternal(std::string filename);

    [[nodiscard]]
    static std::string normalizeFilenameWoW(std::string filename);


  public:
    inline static constexpr std::array<std::string_view, 16> Locales { // Russian was added in TBC, Portugese and Italian were added in WotLK
        "enGB", "enUS", "deDE", "koKR", "frFR", "zhCN", "enCN", "zhTW", "enTW", "esES", "esMX", "ruRU", "jaJP", "ptPT", "ptBR", "itIT"}; // 12 to 15 = unknown

    /* Reverse engineered enUS cleint patch priority
    1.  Archive: Data\alternate.MPQ Priority : 134
    2.  Archive : ..\Data\alternate.MPQ Priority : 134
    3.  Archive : Data\patch - A.MPQ Priority : 133
    4.  Archive : Data\patch - 3.MPQ Priority : 132
    5.  Archive : Data\patch - 2.MPQ Priority : 131
    6.  Archive : Data\enUS\patch - enUS - 3.MPQ Priority : 130
    7.  Archive : Data\enUS\patch - enUS - 2.MPQ Priority : 129
    8.  Archive : Data\patch.MPQ Priority : 128
    9.  Archive : Data\enUS\patch - enUS.MPQ Priority : 127
    10. Archive : Data\base.MPQ Priority : 100
    11. Archive : Data\expansion.MPQ Priority : 49
    12. Archive : Data\lichking.MPQ Priority : 48
    13. Archive : Data\common - 2.MPQ Priority : 46
    14. Archive : Data\common.MPQ Priority : 47
    15. Archive : Data\enUS\locale - enUS.MPQ Priority : 45
    16. Archive : Data\enUS\speech - enUS.MPQ Priority : 44
    17. Archive : Data\enUS\expansion - locale - enUS.MPQ Priority : 43
    18. Archive : Data\enUS\lichking - locale - enUS.MPQ Priority : 42
    19. Archive : Data\enUS\expansion - speech - enUS.MPQ Priority : 41
    20. Archive : Data\enUS\lichking - speech - enUS.MPQ Priority : 40
    */

    // Templates in correct order for opening the wotlk client MPQs
    inline static constexpr std::array<std::string_view, 8> ArchiveNameTemplates{
                                                                                    // common archives
                                                                                      "common.MPQ"
                                                                                    , "common-2.MPQ"
                                                                                    , "lichking.MPQ"
                                                                                    , "expansion.MPQ"
                                                                                    /* "base.MPQ" */
                                                                                    , "patch.MPQ"
                                                                                    , "patch-{number}.MPQ"
                                                                                    , "patch-{character}.MPQ"
                                                                                    , "alternate.MPQ"
    };

    inline static constexpr std::array<std::string_view, 10> LocaleArchiveNameTemplates{

                                                                                    // locale-specific archives
                                                                                      "{locale}/lichking-speech-{locale}.MPQ"
                                                                                    , "{locale}/expansion-speech-{locale}.MPQ"

                                                                                    , "{locale}/lichking-locale-{locale}.MPQ"
                                                                                    , "{locale}/expansion-locale-{locale}.MPQ"

                                                                                    , "{locale}/speech-{locale}.MPQ"
                                                                                    , "{locale}/locale-{locale}.MPQ"

                                                                                    , "{locale}/patch-{locale}.MPQ"
                                                                                    , "{locale}/patch-{locale}-{number}.MPQ" 
                                                                                    , "{locale}/patch-{locale}-{character}.MPQ"

                                                                                    , "development.MPQ"
    };

    // Vanilla 1.12.x (build 5875) base data MPQs. Unlike WotLK there is no single
    // base/expansion archive; data is split by asset type. loadMPQArchive() skips any
    // that are missing, so custom clients (e.g. Turtle WoW) with extra patch-<x>.MPQ
    // archives are covered by the {number}/{character} templates below.
    inline static constexpr std::array<std::string_view, 14> VanillaArchiveNameTemplates{
                                                                                      "base.MPQ" // present on custom clients (e.g. Turtle WoW); skipped if absent
                                                                                    , "dbc.MPQ"
                                                                                    , "fonts.MPQ"
                                                                                    , "interface.MPQ"
                                                                                    , "misc.MPQ"
                                                                                    , "model.MPQ"
                                                                                    , "sound.MPQ"
                                                                                    , "speech.MPQ"
                                                                                    , "terrain.MPQ"
                                                                                    , "texture.MPQ"
                                                                                    , "wmo.MPQ"
                                                                                    , "patch.MPQ"
                                                                                    , "patch-{number}.MPQ"
                                                                                    , "patch-{character}.MPQ"
    };

    inline static constexpr std::array<std::string_view, 6> VanillaLocaleArchiveNameTemplates{
                                                                                      "{locale}/base-{locale}.MPQ"
                                                                                    , "{locale}/locale-{locale}.MPQ"
                                                                                    , "{locale}/speech-{locale}.MPQ"
                                                                                    , "{locale}/patch-{locale}.MPQ"
                                                                                    , "{locale}/patch-{locale}-{number}.MPQ"
                                                                                    , "{locale}/patch-{locale}-{character}.MPQ"
    };

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> _minimap_md5translate;

  private:

    void initializeMPQStorage();
    void loadMPQArchive(std::string const& mpq_path);
    void initializeCASCStorage();
    void validateLocale();

    OpenMode _open_mode;
    StorageType _storage_type;
    ClientVersion _version;
    Locale _locale_mode;
    std::string _path; // client path
    std::string _local_path; // project path
    std::optional<std::string> _cdn_cache_path;

    // A sorted list of loaded archives. The last one is the most up-to-date one.
    std::vector<Archive::BaseArchive*> _archives;
    Listfile::Listfile _listfile;

    // sync
    std::mutex _mutex;


  };
}

#endif // BLIZZARD_ARCHIVE_CLIENT_DATA_HPP