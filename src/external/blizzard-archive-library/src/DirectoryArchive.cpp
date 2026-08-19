
#include <DirectoryArchive.hpp>
#include <filesystem>
#include <cassert>

namespace fs = std::filesystem;
using namespace BlizzardArchive::Archive;
using namespace BlizzardArchive::Listfile;

DirectoryArchive::DirectoryArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile)
: BaseArchive(path, locale, listfile)
{

}

std::string DirectoryArchive::getNormalizedFilepath(Listfile::FileKey const& file_key) const
{
  fs::path local_path;

  if (file_key.hasFilepath())
  {
    local_path = fs::path(_path) / ClientData::normalizeFilenameUnix(file_key.filepath());
  }
  else
  {
    // try deducing filepath from listfile
    assert(file_key.hasFileDataID());
    std::string filepath = _listfile->getPath(file_key.fileDataID());

    if (!filepath.empty())
    {
      local_path =  fs::path(_path) / ClientData::normalizeFilenameUnix(filepath);
    }
    else
    {
      return "";
    }
  }

  if (!fs::exists(local_path))
    return "";

  return std::move(local_path.string());
}

bool DirectoryArchive::openFile(FileKey const& file_key, Locale locale, HANDLE* file_handle) const
{
  std::string file_path = getNormalizedFilepath(file_key);

  if (file_path.empty())
    return false;

  HANDLE handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(OpenFilesManager::instance()->openFiles().size()));

  std::ifstream stream {file_path, std::ios_base::binary | std::ios_base::in};

  if (!stream.is_open())
    return false;

  OpenFilesManager::instance()->openFiles().insert(std::pair<HANDLE, std::ifstream>{handle,
    std::move(stream)});

  return true;
}

bool DirectoryArchive::readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const
{
  std::ifstream& stream = OpenFilesManager::instance()->openFiles()[file_handle];
  stream.seekg(0, std::ios::beg);
  stream.read(buffer, buf_size);

  return true;
}

bool DirectoryArchive::closeFile(HANDLE file_handle) const
{
  auto& open_files = OpenFilesManager::instance()->openFiles();
  auto it = open_files.find(file_handle);

  if (it == open_files.end())
    return false;

  std::ifstream& stream = it->second;
  stream.close();
  open_files.erase(it);
  return true;
}


std::uint64_t DirectoryArchive::getFileSize(HANDLE file_handle) const
{
  std::ifstream& stream = OpenFilesManager::instance()->openFiles()[file_handle];
  stream.seekg(0, std::ios::end);
  return stream.tellg();
}

bool DirectoryArchive::exists(Listfile::FileKey const& file_key, Locale locale) const
{
  std::string file_path = getNormalizedFilepath(file_key);

  if (file_path.empty())
    return false;

  return true;
}

DirectoryArchive::~DirectoryArchive()
{
  // safety check to release fs descriptors in case of exception or anything
  auto& open_files = OpenFilesManager::instance()->openFiles();

  for (auto& file : open_files)
  {
    file.second.close();
  }

  open_files.clear();
}

