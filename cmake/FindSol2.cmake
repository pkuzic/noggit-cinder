# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# Dependency: sol2
FetchContent_Declare (sol2
        GIT_REPOSITORY https://github.com/tswow/sol2
        GIT_TAG b9c83d5ecf6bc9503dc66779f2395dc32dffb1e5
        )
FetchContent_MakeAvailable (sol2)
# sol2::sol2 neither links lua nor sets include directories as system so will clobber us with
# loads of warnings, sadly. It also wants to be install(EXPORT)ed which is not what we want.
add_library (sane-sol2 INTERFACE)
add_library (sol2::sane ALIAS sane-sol2)
target_link_libraries (sane-sol2 INTERFACE Lua::Lua)
target_include_directories (sane-sol2 SYSTEM INTERFACE "${sol2_SOURCE_DIR}/include")
