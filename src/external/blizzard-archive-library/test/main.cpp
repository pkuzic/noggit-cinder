#include <ClientData.hpp>
#include <ClientFile.hpp>

#include <iostream>

using namespace BlizzardArchive;

int main(int argc, char* argv[])
{
  auto proj_path = std::string("/home/skarn/Desktop/test_proj/");

  // MPQ storage tests
  {  
    auto directory_path = std::string("/media/skarn/NTFS/WoWModding/World of Warcraft 3.3.5a/");
    auto wow_fs = ClientData(directory_path, ClientVersion::WOTLK, Locale::AUTO, proj_path);

    auto file = BlizzardArchive::ClientFile(BlizzardArchive::Listfile::FileKey("world/wmo/azeroth/buildings/human_farm/farm.wmo"), &wow_fs);
    file.save();
  }

  // Local CASC storage tests
  {
    //auto directory_path = std::string("D:\\World of Warcraft");
    auto directory_path = std::string("/media/skarn/Boot Camp/World of Warcraft/");

    auto wow_fs = BlizzardArchive::ClientData(directory_path, ClientVersion::SL, Locale::enUS, proj_path);

    auto file = BlizzardArchive::ClientFile(Listfile::FileKey("sound/music/citymusic/darnassus/darnassus intro.mp3"), &wow_fs);
    file.save();

    auto file1 = BlizzardArchive::ClientFile(Listfile::FileKey(130497), &wow_fs);
    file1.save();
  }

  // Remote CASC storage tests
  {
    //auto directory_path = std::string("D:\\World of Warcraft");
    auto directory_path = std::string("/home/skarn/Desktop/cdn_cache_test/");

    auto wow_fs = BlizzardArchive::ClientData("http://%s.falloflordaeron.com:8000/%s/%s", directory_path, ClientVersion::SL, Locale::enUS, proj_path);

    auto file = BlizzardArchive::ClientFile(Listfile::FileKey("sound/music/citymusic/orgrimmar/orgrimmar01-moment.mp3"), &wow_fs);
    file.save();

    auto file1 = BlizzardArchive::ClientFile(Listfile::FileKey(53198), &wow_fs);
    file1.save();
  }




  return 0;
}