#include <ClientData.hpp>
#include <Exception.hpp>
#include <MPQArchive.hpp>
#include <DirectoryArchive.hpp>
#include <CASCArchive.hpp>

#include <StormLib.h>

#include <filesystem>
#include <cassert>
#include <regex>

using namespace BlizzardArchive;
namespace fs = std::filesystem;

ClientData::ClientData(std::string const& path, ClientVersion version, Locale locale, std::string const& local_path)
  : _version(version)
  , _open_mode(OpenMode::LOCAL)
  , _storage_type((version == ClientVersion::SL) ? StorageType::CASC : StorageType::MPQ) // WOTLK + CLASSIC are MPQ; only SL (and future retail) use CASC
  , _locale_mode(locale)
  , _path(path)
  , _local_path(ClientData::normalizeFilenameUnix(local_path))
{

  validateLocale();

  switch (_storage_type)
  {
  case StorageType::MPQ:
    initializeMPQStorage();
    break;
  case StorageType::CASC:
    initializeCASCStorage();
    break;
  }
}

ClientData::ClientData(std::string const& path, std::string const& cdn_cache_path, ClientVersion version, Locale locale, std::string const& local_path)
    : _version(version)
    , _open_mode(OpenMode::REMOTE)
    , _storage_type((version == ClientVersion::SL) ? StorageType::CASC : StorageType::MPQ) // WOTLK + CLASSIC are MPQ; only SL (and future retail) use CASC
    , _locale_mode(locale)
    , _path(path)
    , _local_path(ClientData::normalizeFilenameUnix(local_path))
    , _cdn_cache_path(cdn_cache_path)
{

  validateLocale();

  switch (_storage_type)
  {
    case StorageType::CASC:
      initializeCASCStorage();
      break;
    case StorageType::MPQ:
      throw Exceptions::Archive::ArchiveOpenError("MPQ storage does not support online loading.");
      break;
  }
}

ClientData::~ClientData()
{
  // clean up
  for (auto archive : _archives)
  {
    delete archive;
  }
}

bool BlizzardArchive::ClientData::mpqArchiveExistsOnDisk(std::string const& archive_name)
{
    std::string mpq_path = (fs::path(_path) / "Data" / archive_name).string();

    std::string::size_type location(std::string::npos);
    std::string_view const& locale = locale_name();

    if (!fs::exists(mpq_path))
        return false;

    if (fs::is_directory(mpq_path))
    {
        return true; // directory patch. special return?
    }
    else
    {
        return true;
    }
}

// archive_name = patch-4.MPQ, must include extension
std::optional<Archive::MPQArchive*> BlizzardArchive::ClientData::getMPQArchive(std::string const& archive_name)
{
    if (_storage_type != StorageType::MPQ)
        return std::nullopt;

    const std::lock_guard _lock(_mutex);

    // case sensitive
    std::string mpq_path = (fs::path(_path) / "Data" / archive_name).string();

    for (auto* archive_ptr : _archives)
    {
        // full disk paths.
        if (archive_ptr->path() == mpq_path)
        {
            if (Archive::MPQArchive* mpqArchive_ptr = dynamic_cast<Archive::MPQArchive*>(archive_ptr))
            {
                return mpqArchive_ptr;
            }
        }
    }

    // if not in memory, try to get it on disk
    if (mpqArchiveExistsOnDisk(archive_name))
    {
        Archive::MPQArchive* new_archive = new Archive::MPQArchive(mpq_path, _locale_mode, &_listfile);
        if (new_archive)
        {
            _archives.push_back(new_archive);
            return new_archive;
        }
    }

    return std::nullopt;
}

std::optional<Archive::MPQArchive*> BlizzardArchive::ClientData::tryCreateMPQArchive(std::string const& archive_name)
{
    if (_storage_type != StorageType::MPQ)
        return std::nullopt;

    // check if there is already a file
    std::string mpq_path = (fs::path(_path) / "Data" / archive_name).string();

    std::string::size_type location(std::string::npos);
    std::string_view const& locale = locale_name();

    if (!fs::exists(mpq_path))
    {
        // create archive here or in MPQ class ?
        HANDLE hMpq = NULL;
        // NB: SFileAddFileEx SILENTLY drops files once the hash table is full -- size it with
        // plenty of headroom (a full 64x64 continent is ~4k ADTs before minimaps/DBCs).
        unsigned long dwMaxFileCount = 0x8000;// 0x1000 seems to be the default // 0x4 is the minimum

        bool const is_classic = (_version == ClientVersion::CLASSIC);

        // Match what real 1.12 patch archives look like. Turtle's own patches contain ONLY data
        // files -- no (listfile), no (attributes). StormLib writes those two ENCRYPTED by
        // default, which is the one structural difference an exported patch had from a stock
        // one; drop them for classic so an exported archive is byte-shaped like a native patch.
        // (Format v1 is kept for classic -- vanilla-era clients read it; Turtle reads both.)
        unsigned long dwCreateFlags =
            (is_classic ? MPQ_CREATE_ARCHIVE_V1
                        : (MPQ_CREATE_LISTFILE | MPQ_CREATE_ATTRIBUTES | MPQ_CREATE_ARCHIVE_V2));

        if (SFileCreateArchive(mpq_path.c_str(), dwCreateFlags, dwMaxFileCount, &hMpq))
        {
            if (!SFileCloseArchive(hMpq))
            {
                DWORD error = GetLastError();
                throw Exceptions::Archive::ArchiveCloseError("ClientData::tryCreateMPQArchive(): Error closing archive: " + mpq_path);
            }

            // now we can initialize the class and open it in read-only mode
            Archive::MPQArchive* new_archive = new Archive::MPQArchive(mpq_path, _locale_mode, &_listfile);
            if (new_archive)
            {
                _archives.push_back(new_archive);
                return new_archive;
            }
        }
        else
        {
            // failed to create archive
            DWORD error = GetLastError();
            throw Exceptions::Archive::ArchiveOpenError("ClientData::tryCreateMPQArchive(): Error creating archive: " + mpq_path, error);
        }
    }
    return std::nullopt;
}

bool BlizzardArchive::ClientData::isMPQNameValid(std::string const& archive_name, bool exclude_base_mpqs)
{
    // Make sure archive_name is lowercase!

    // if MPQ isn't in the list of allowed MPQs, return false
    // if exclude_base_mpqs, return false if the MPQ is present in the base 3.3.5 client. (eg patch-3 would be wrong, patch-4 would be allowed)

    int const clientPatchId = 3; // 3.[3].5

    // this doesn't include locale!
    for (auto const& filename : ClientData::ArchiveNameTemplates)
    {

        std::string mpq_path = (fs::path(_path) / "Data" / filename).string();

        std::string::size_type location(std::string::npos);
        std::string_view const& locale = locale_name();

        do
        {
            location = mpq_path.find("{locale}");
            if (location != std::string::npos)
            {
                mpq_path.replace(location, 8, locale);
            }
        } while (location != std::string::npos);

        if (mpq_path.find("{number}") != std::string::npos)
        {
            location = mpq_path.find("{number}");
            mpq_path.replace(location, 8, " ");
            for (char j = '2'; j <= '9'; j++)
            {
                mpq_path.replace(location, 1, std::string(&j, 1));

                // hack to lowercase the extension
                auto mpq_filename = fs::path(mpq_path).filename().replace_extension(".mpq").string();
                if (archive_name == mpq_filename)
                {
                    // convert j from char to int
                    if (exclude_base_mpqs && (j - '0') <= 3)
                        return false;
                    else
                        return true;
                }
            }
        }
        else if (mpq_path.find("{character}") != std::string::npos)
        {
            location = mpq_path.find("{character}");
            mpq_path.replace(location, 11, " ");
            for (char c = 'a'; c <= 'z'; c++)
            {
                mpq_path.replace(location, 1, std::string(&c, 1));
                
                // all letter patches are valid.
                auto mpq_filename = fs::path(mpq_path).filename().replace_extension(".mpq").string();
                if (archive_name == mpq_filename)
                {
                    return true;
                }
            }
        }
        else
        {
            if (archive_name == fs::path(mpq_path).filename().replace_extension(".mpq").string())
                return exclude_base_mpqs ? false : true;
        }
    }
    // name was not in the list of allowed names.
    return false;
}

void ClientData::loadMPQArchive(std::string const& mpq_path)
{
  if (!fs::exists(mpq_path) || fs::equivalent(mpq_path, _local_path))
    return;

  if (fs::is_directory(mpq_path))
  {
    _archives.push_back(new Archive::DirectoryArchive(mpq_path, _locale_mode, &_listfile));
  }
  else
  {
    _archives.push_back(new Archive::MPQArchive(mpq_path, _locale_mode, &_listfile));
  }


}

void ClientData::initializeMPQStorage()
{

    std::string_view const& locale = locale_name();

    // Re order the archives based on locale.
    // TODO : Confirms it is only those two clients

    std::vector<std::string_view> OrderedArchiveNameTemplates;
    OrderedArchiveNameTemplates.reserve(24);

    auto const append_templates = [&OrderedArchiveNameTemplates](auto const& templates)
    {
        for (auto const& filename : templates)
            OrderedArchiveNameTemplates.push_back(filename);
    };

    // for enGB and enUS clients, load locale before Data
    bool const locale_first = (locale == "enGB" || locale == "enUS");

    if (_version == ClientVersion::CLASSIC)
    {
        if (locale_first)
        {
            append_templates(ClientData::VanillaLocaleArchiveNameTemplates);
            append_templates(ClientData::VanillaArchiveNameTemplates);
        }
        else
        {
            append_templates(ClientData::VanillaArchiveNameTemplates);
            append_templates(ClientData::VanillaLocaleArchiveNameTemplates);
        }
    }
    else
    {
        if (locale_first)
        {
            append_templates(ClientData::LocaleArchiveNameTemplates);
            append_templates(ClientData::ArchiveNameTemplates);
        }
        else
        {
            append_templates(ClientData::ArchiveNameTemplates);
            append_templates(ClientData::LocaleArchiveNameTemplates);
        }
    }

  for (auto const& filename : OrderedArchiveNameTemplates)
  {
    std::string mpq_path = (fs::path(_path) / "Data" / filename).string();

    std::string::size_type location(std::string::npos);

    do
    {
      location = mpq_path.find("{locale}");
      if (location != std::string::npos)
      {
        mpq_path.replace(location, 8, locale);
      }
    } while (location != std::string::npos);

    if (mpq_path.find("{number}") != std::string::npos)
    {
      location = mpq_path.find("{number}");
      mpq_path.replace(location, 8, " ");
      for (char j = '2'; j <= '9'; j++)
      {
        mpq_path.replace(location, 1, std::string(&j, 1));
        loadMPQArchive(mpq_path);
      }
    }
    else if (mpq_path.find("{character}") != std::string::npos)
    {
      location = mpq_path.find("{character}");
      mpq_path.replace(location, 11, " ");
      for (char c = 'a'; c <= 'z'; c++)
      {
        mpq_path.replace(location, 1, std::string(&c, 1));
        loadMPQArchive(mpq_path);
      }
    }
    else
    {
      loadMPQArchive(mpq_path);
    }
  }
}

void ClientData::initializeCASCStorage()
{
  _listfile.initFromCSV((fs::path(_local_path) / "listfile.csv").string());

  switch (_open_mode)
  {
    case OpenMode::LOCAL:
    {
      _archives.push_back(new Archive::CASCArchive(_path, "", _locale_mode, _open_mode, &_listfile));
      break;
    }
    case OpenMode::REMOTE:
    {
      assert(_cdn_cache_path.has_value());
      _archives.push_back(new Archive::CASCArchive(_path, _cdn_cache_path.value(), _locale_mode, _open_mode, &_listfile));
      break;
    }
  }

}

void ClientData::validateLocale()
{
  switch (_storage_type)
  {
    case StorageType::MPQ:
    {
      if (static_cast<int>(_locale_mode)) // manual locale
      [[unlikely]]
      {
        fs::path realmlist_path = fs::path(_path) / "Data"
            / locale_name() / "realmlist.wtf";

        if (!fs::exists(realmlist_path))
        {
          throw Exceptions::Locale::LocaleNotFoundError("Requested locale \""
            + std::string(locale_name().data()) +
            "\" does not exist in the client directory."
            "Be sure, that there is one containing the file \"realmlist.wtf\".");
        }

      }
      else // auto locale
      [[likely]]
      {
        for (unsigned i = 0; i < ClientData::Locales.size(); ++i)
        {
          fs::path realmlist_path = fs::path(_path) / "Data" / ClientData::Locales[i] / "realmlist.wtf";

          if (fs::exists(realmlist_path))
          {
            _locale_mode = static_cast<Locale>(i + 1);
            return;
          }
        }

        // Many custom/private vanilla clients (e.g. Turtle WoW) are locale-less: everything
        // lives in top-level MPQs with no Data/<locale>/ directory and realmlist.wtf sits at
        // the client root. Don't fail there — fall back to enUS (locale id 0 for DBC strings);
        // the locale-specific MPQ templates simply resolve to nothing and are skipped.
        if (_version == ClientVersion::CLASSIC)
        {
          _locale_mode = Locale::enUS;
          return;
        }

        throw Exceptions::Locale::LocaleNotFoundError("Automatic locale detection failed. "
                                                      "The client directory does not contain any locale directory. Be "
                                                      "sure, that there is one containing the file \"realmlist.wtf\".");
      }
      break;
    }
    case StorageType::CASC:
    {
      if (_locale_mode == Locale::AUTO)
      {
        throw Exceptions::Locale::IncorrectLocaleModeError("Automatic locale detection is not"
                                                           " supported for CASC-based clients.");
      }
      break;
    }
  }
}

bool ClientData::readFile(Listfile::FileKey const& file_key, std::vector<char>& buffer)
{
  const std::lock_guard _lock(_mutex);

  HANDLE file_handle = nullptr;

  // largest sane client file -- vanilla/WotLK assets top out far below this. Anything bigger
  // means the size query failed or the archive entry is corrupt.
  constexpr std::uint64_t max_sane_file_size = 512ull * 1024ull * 1024ull;

  for (auto it = _archives.rbegin(); it != _archives.rend(); ++it)
  {
    if (!(*it)->openFile(file_key, _locale_mode, &file_handle))
      continue;

    std::uint64_t buf_size = (*it)->getFileSize(file_handle);

    // SFileGetFileSize returns SFILE_INVALID_SIZE (0xFFFFFFFF) on FAILURE. Treating that as a
    // real size allocated a 4 GB buffer (multi-second freeze from zeroing it, or bad_alloc),
    // and the subsequent read scribbled decompressed sectors at wild offsets -> heap
    // corruption. This fired constantly during tile streaming: Turtle map data references
    // WotLK-only files absent from the 1.12 client (e.g. demolishercannonball.m2), whose
    // open/size queries fail down this path. Skip empty, invalid and absurd sizes.
    if (!buf_size || buf_size == 0xFFFFFFFFull || buf_size > max_sane_file_size)
    {
      (*it)->closeFile(file_handle);
      continue;
    }

    buffer.resize(buf_size);

    if (!(*it)->readFile(file_handle, buffer.data(), buf_size))
    {
      // failed/partial read: do NOT hand the caller a garbage buffer (the old assert was
      // compiled out in release builds, silently returning junk as a "loaded" file).
      (*it)->closeFile(file_handle);
      buffer.clear();
      continue;
    }

    if (!(*it)->closeFile(file_handle))
    {
      assert(false);
    }

    return true;
  }

  return false;
}

std::array<int, 2> BlizzardArchive::ClientData::saveLocalFilesToArchive(Archive::MPQArchive* archive, bool compress, bool compact)
{
    // Only supports MPQ currently.
    if (_storage_type != StorageType::MPQ)
        return {};

    const std::lock_guard _lock(_mutex);

    int file_count = 0;
    int file_success = 0;
    std::array<int, 2> result_array = {0,0};

    try {
        // Check if the given path is a directory
        if (!fs::is_directory(_local_path)) {
            return result_array;
        }

        if (!archive->openForWritting())
        {
            return result_array;
        }


        // Iterate through all files in the directory
        for (const auto& entry : fs::recursive_directory_iterator(_local_path)) {
            if (entry.is_regular_file()) {

                std::string const extension = entry.path().extension().string();
                // filter noggit files
                if (extension == ".noggitproj" || extension == ".json" || extension == ".ini")
                    continue;

                file_count++;

                // use SFileAddFileEx high level function which does everything, or read manually
                bool full_write_file = false;

                if (!full_write_file)
                {
                    // get internal Wow path
                    auto wow_path = fs::relative(entry.path(), fs::path(_local_path)).generic_string();

                    try
                    {
                        // normalize path with filekey or nah ?
                        bool success = archive->addFile(wow_path, entry.path().generic_string(), _locale_mode, 0, compress);
                        if (success)
                            file_success++;
                        else
                            continue;
                    }
                    catch (...)
                    {
                        continue;
                    }
                }
                else
                {
                    // Open the file
                    std::ifstream file_stream(entry.path(), std::ios_base::binary | std::ios_base::in);
                    if (file_stream.is_open())
                    {
                        // Get the file size
                        file_stream.seekg(0, std::ios::end);
                        std::size_t buf_size = file_stream.tellg();
                        file_stream.seekg(0, std::ios::beg);

                        // Allocate buffer
                        char* buffer = new char[buf_size];
                        // Read file into buffer
                        file_stream.read(buffer, buf_size);
                        file_stream.close();

                        // get internal Wow path
                        auto wow_path = fs::relative(entry.path(), fs::path(_local_path)).generic_string();
                        try
                        {
                            bool success = archive->writeFile(wow_path, buffer, buf_size, _locale_mode, 0, compress);
                            if (success)
                                file_success++;
                            else
                                continue;
                        }
                        catch (...)
                        {
                            continue;
                        }

                        delete[] buffer;
                    }
                    else {
                        continue; // std::cerr << "Unable to open file: " << entry.path() << std::endl;
                    }
                }
            }
        }

        // compact if at least 1? file got added.
        if (compact && file_success >= 1)
        {
            archive->compactArchive();
        }

        // close archive back to read-only
        archive->closeToReadOnly();
    }
    catch (const fs::filesystem_error& ex) {
        // std::cerr << "Filesystem error: " << ex.what() << std::endl;
        return result_array;
    }
    catch (...)
    {
        archive->closeToReadOnly();
    }

    return result_array = {file_count, file_success};
}

bool ClientData::existsOnDisk(Listfile::FileKey const& file_key)
{
  if (!file_key.hasFilepath())
    return false;

  return fs::exists(getDiskPath(file_key));
}

bool ClientData::exists(Listfile::FileKey const& file_key)
{
  if (ClientData::existsOnDisk(file_key))
  {
    return true;
  }

  const std::lock_guard _lock(_mutex);

  for (auto it = _archives.rbegin(); it != _archives.rend(); ++it)
  {
    if ((*it)->exists(file_key, _locale_mode))
      return true;
  }

  return false;
}

int const BlizzardArchive::ClientData::getLocaleId() const
{
    switch (_locale_mode)
    {
    case BlizzardArchive::Locale::AUTO:
    case BlizzardArchive::Locale::enGB:
    case BlizzardArchive::Locale::enUS:
        return static_cast<int>(LocaleLang::enUS);
    case BlizzardArchive::Locale::koKR:
        return static_cast<int>(LocaleLang::koKR);
    case BlizzardArchive::Locale::frFR:
        return static_cast<int>(LocaleLang::frFR);
    case BlizzardArchive::Locale::deDE:
        return static_cast<int>(LocaleLang::deDE);
    case BlizzardArchive::Locale::zhCN:
    case BlizzardArchive::Locale::enCN:
        return static_cast<int>(LocaleLang::enCN);
    case BlizzardArchive::Locale::zhTW:
    case BlizzardArchive::Locale::enTW:
        return static_cast<int>(LocaleLang::enTW);
    case BlizzardArchive::Locale::esES:
    case BlizzardArchive::Locale::esMX:
        return static_cast<int>(LocaleLang::esES);
    case BlizzardArchive::Locale::ruRU:
        return static_cast<int>(LocaleLang::ruRU);
    case BlizzardArchive::Locale::jaJP:
        return static_cast<int>(LocaleLang::jaJP);
    case BlizzardArchive::Locale::ptPT:
    case BlizzardArchive::Locale::ptBR:
        return static_cast<int>(LocaleLang::ptPT);
    case BlizzardArchive::Locale::itIT:
        return static_cast<int>(LocaleLang::itIT);
    default:
        throw Exceptions::Locale::IncorrectLocaleModeError("Unsupported locale mode");
    }
}

std::string ClientData::getDiskPath(Listfile::FileKey const& file_key)
{
  const std::lock_guard _lock(_mutex);

  if (file_key.hasFilepath())
  {
    return (fs::path(_local_path) / ClientData::normalizeFilenameUnix(file_key.filepath())).string();
  }
  else
  {
    // try deducing filepath from listfile
    assert(file_key.hasFileDataID());
    std::string filepath = _listfile.getPath(file_key.fileDataID());

    if (!filepath.empty())
    {
      return (fs::path(_local_path) / ClientData::normalizeFilenameUnix(filepath)).string();
    }
    else
    {
      return (fs::path(_local_path) / "unknown_files/" / std::to_string(file_key.fileDataID())).string();
    }
  }
   
}

std::string ClientData::normalizeFilenameUnix(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin()
    , [](char c)
    {
      return c == '\\' ? '/' : c;
    }
  );
  return filename;
}

std::string ClientData::normalizeFilenameInternal(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
  std::transform(filename.begin(), filename.end(), filename.begin()
      , [](char c)
                 {
                   return c == '\\' ? '/' : c;
                 }
  );

  if (filename.ends_with(".mdx"))
  {
    filename = std::regex_replace(filename, std::regex(".mdx"), ".m2");
  }
  else if(filename.ends_with(".mdl"))
  {
    filename = std::regex_replace(filename, std::regex(".mdl"), ".m2");
  }

  return filename;
}

std::string ClientData::normalizeFilenameWoW(std::string filename)
{
  std::transform(filename.begin(), filename.end(), filename.begin(), ::toupper);
  std::transform(filename.begin(), filename.end(), filename.begin()
    , [](char c)
    {
      return c == '/' ? '\\' : c;
    }
  );
  return filename;
}