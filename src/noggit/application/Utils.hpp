// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#ifndef NOGGIT_UTILS_HPP
#define NOGGIT_UTILS_HPP

#include <noggit/application/NoggitApplication.hpp>

#include <ClientFile.hpp>
#include <stream/StreamReader.h>


inline auto readFileAsIMemStream = [](std::string const& file_path) -> std::shared_ptr<BlizzardDatabaseLib::Stream::IMemStream>
{
  try
  {
    BlizzardArchive::ClientFile f(file_path, Noggit::Application::NoggitApplication::instance()->clientData());

    return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(f.getBuffer(), f.getSize());
  }
  catch (...)
  {
    // The DBC doesn't exist in this client (e.g. MapDifficulty is WotLK+, absent in vanilla).
    // ClientFile's constructor throws FileReadFailedError; uncaught, that crashes the app when a
    // caller loads such a table. Hand back a minimal valid *empty* WDBC file instead, so the
    // table just loads as empty (0 records).
    static char const empty_wdbc[20] = { 'W', 'D', 'B', 'C', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    return std::make_shared<BlizzardDatabaseLib::Stream::IMemStream>(empty_wdbc, sizeof(empty_wdbc));
  }
};


#endif //NOGGIT_UTILS_HPP
