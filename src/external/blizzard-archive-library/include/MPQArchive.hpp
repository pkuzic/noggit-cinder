#ifndef BLIZZARDARCHIVE_MPQARCHIVE_HPP
#define BLIZZARDARCHIVE_MPQARCHIVE_HPP

#include <BaseArchive.hpp>

namespace BlizzardArchive::Listfile
{
  class Listfile;
}

namespace BlizzardArchive::Archive
{

  class MPQArchive : public BaseArchive
  {
  public:
    MPQArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile);
    ~MPQArchive() override;

    // static MPQArchive createNew();

    [[nodiscard]]
    bool openFile(Listfile::FileKey const& file_key, Locale locale, HANDLE* file_handle) const override;
    [[nodiscard]]
    bool writeFile(Listfile::FileKey const& wow_path, const char* file_data, std::size_t buf_size,
        Locale locale, unsigned long skip_error = 0, bool compress = true); // override;
    [[nodiscard]]
    bool addFile(Listfile::FileKey const& wow_path, std::string file_disk_path,
        Locale locale, unsigned long skip_error = 0, bool compress = true); // override;


    bool compactArchive() const;
    
    bool readFile(HANDLE file_handle, char* buffer, std::size_t buf_size) const override;
    bool closeFile(HANDLE file_handle) const override;

    // close the archive and reopen it without read-only flag to allow writting.
    [[nodiscard]]
    bool openForWritting();
    // Call after writting operations. close and reopen with read-only flags.
    bool closeToReadOnly();

    [[nodiscard]]
    std::uint64_t getFileSize(HANDLE file_handle) const override;

    [[nodiscard]]
    bool exists(Listfile::FileKey const& file_key, Locale locale) const override;

  private:
    bool flushArchive() const;

    HANDLE _handle = nullptr;
  };
}

#endif // BLIZZARDARCHIVE_MPQARCHIVE_HPP