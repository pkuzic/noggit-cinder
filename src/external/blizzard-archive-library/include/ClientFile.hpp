#ifndef BLIZZARDARCHIVE_CLIENTFILE_HPP
#define BLIZZARDARCHIVE_CLIENTFILE_HPP

#include <ClientData.hpp>
#include <BaseArchive.hpp>
#include <filesystem>

namespace BlizzardArchive
{

  namespace Listfile
  {
    class FileKey;
  }

  class ClientFile
  {
  public:

    struct NEW_FILE_T {};
    inline static constexpr NEW_FILE_T NEW_FILE {};

    explicit ClientFile(Listfile::FileKey const& file_key, ClientData* client_data);
    explicit ClientFile(Listfile::FileKey const& file_key, ClientData* client_data, NEW_FILE_T);

    ClientFile() = delete;
    ClientFile(ClientFile const&) = delete;
    ClientFile(ClientFile&&) = delete;
    ClientFile& operator=(ClientFile const&) = delete;
    ClientFile& operator=(ClientFile&&) = delete;

    std::size_t read(void* dest, std::size_t bytes);

    [[nodiscard]]
    std::size_t getSize() const;

    [[nodiscard]]
    std::size_t getPos() const;

    [[nodiscard]]
    char const* getBuffer() const;

    [[nodiscard]]
    char const* getPointer() const;

    [[nodiscard]]
    bool isEof() const;

    void seek(std::size_t offset);
    void seekRelative(std::size_t offset);
    void close();

    [[nodiscard]]
    bool isExternal() const
    {
      return _external;
    }

    template<typename T>
    const T* get(size_t offset) const
    {
      return reinterpret_cast<T const*>(_buffer.data() + offset);
    }

    void setBuffer(std::vector<char> const& vec) { _buffer = vec; }

    void save();

  private:
    bool _eof;
    std::vector<char> _buffer;
    size_t _pointer;
    bool _external;
    std::filesystem ::path _disk_path;
    Listfile::FileKey _file_key;
    

  };
}

#endif // BLIZZARDARCHIVE_CLIENTFILE_HPP