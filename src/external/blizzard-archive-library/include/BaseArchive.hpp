#ifndef BLIZZARDARCHIVE_BASEARCHIVE_HPP
#define BLIZZARDARCHIVE_BASEARCHIVE_HPP

#include <ClientData.hpp>
#include <cstdint>

namespace BlizzardArchive::Listfile
{
  class Listfile;
}

namespace BlizzardArchive::Archive
{

  class BaseArchive
  {
  public:
    BaseArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile);
    virtual ~BaseArchive() = default;

    [[nodiscard]]
    std::string const& path() const { return _path; };

    [[nodiscard]]
    virtual bool openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const = 0;

    virtual bool readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const = 0;
    virtual bool closeFile(HANDLE file_handle) const = 0;

    [[nodiscard]]
    virtual std::uint64_t getFileSize(HANDLE file_handle) const = 0;

    [[nodiscard]]
    virtual bool exists(Listfile::FileKey const& file_key, Locale locale) const = 0;

  protected:
    std::string _path;
    Locale _locale;
    Listfile::Listfile* _listfile;
  };
}

#endif // BLIZZARDARCHIVE_BASEARCHIVE_HPP