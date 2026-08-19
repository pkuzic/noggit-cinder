#ifndef BLIZZARD_ARCHIVE_EXCEPTION_HPP
#define BLIZZARD_ARCHIVE_EXCEPTION_HPP

#include <stdexcept>

namespace BlizzardArchive::Exceptions
{
  namespace Locale
  {
    class LocaleNotFoundError : public std::runtime_error
    {
    public:
      LocaleNotFoundError(const std::string& what = "") : std::runtime_error(what) {}
    };

    class IncorrectLocaleModeError : public std::runtime_error
    {
    public:
      IncorrectLocaleModeError(const std::string& what = "") : std::runtime_error(what) {}
    };
  }

  namespace Archive
  {
    class ArchiveOpenError : public std::runtime_error
    {
    public:
      ArchiveOpenError(const std::string& what = "", int error_code = 0) : std::runtime_error(what) {}
    };

    class ArchiveCloseError : public std::runtime_error
    {
    public:
        ArchiveCloseError(const std::string& what = "") : std::runtime_error(what) {}
    };

    class FileWriteFailedError : public std::runtime_error
    {
    public:
        FileWriteFailedError(const std::string& what = "", int error_code = 0) : std::runtime_error(what + "\nError code:" + std::to_string(error_code)) {}
    };
  }

  namespace Listfile
  {
    class ListfileNotFoundError : public std::runtime_error
    {
    public:
      ListfileNotFoundError(const std::string& what = "listfile.csv was not found by the provided path!") : std::runtime_error(what) {}
    };
  }

  class FileReadFailedError : public std::runtime_error
  {
  public:
    FileReadFailedError(const std::string& what = "") : std::runtime_error(what) {}
  };

}

#endif // BLIZZARD_ARCHIVE_EXCEPTION_HPP