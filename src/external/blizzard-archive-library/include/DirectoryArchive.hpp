#ifndef NOGGIT_DIRECTORYARCHIVE_HPP
#define NOGGIT_DIRECTORYARCHIVE_HPP

#include "BaseArchive.hpp"
#include <unordered_map>
#include <fstream>

namespace BlizzardArchive::Archive
{
  class OpenFilesManager
  {
  public:
    static OpenFilesManager* instance()
    {
      static OpenFilesManager instance;
      return &instance;
    }

    std::unordered_map<HANDLE, std::ifstream>& openFiles() { return _open_files; };

  private:
    OpenFilesManager() = default;

    std::unordered_map<HANDLE, std::ifstream> _open_files;
  };

  class DirectoryArchive : public BaseArchive
  {
  public:
    DirectoryArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile);
    ~DirectoryArchive();

    [[nodiscard]]
    bool openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const override;

    bool readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const override;
    bool closeFile(HANDLE file_handle) const override;

    [[nodiscard]]
    std::uint64_t getFileSize(HANDLE file_handle) const override;

    [[nodiscard]]
    bool exists(Listfile::FileKey const& file_key, Locale locale) const override;

  private:
    // Returns empty string if local file does not exist
    std::string getNormalizedFilepath(Listfile::FileKey const& file_key) const;
  };

}
#endif //NOGGIT_DIRECTORYARCHIVE_HPP
