#include <BaseArchive.hpp>
#include <Listfile.hpp>

using namespace BlizzardArchive::Archive;

BaseArchive::BaseArchive(std::string const& path, Locale locale, Listfile::Listfile* listfile)
  : _locale(locale)
  , _path(path)
  , _listfile(listfile)
{
}