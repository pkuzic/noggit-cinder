# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# Dependency: json.hpp
# FetchContent_Declare (json
#   GIT_REPOSITORY https://github.com/ArthurSonzogni/nlohmann_json_cmake_fetchcontent
#   GIT_TAG v3.9.1
# )
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json) # replaces FetchContent_PopulateFast

# FetchContent_GetProperties (json)
# IF(NOT json_POPULATED)
#   MESSAGE(STATUS "Installing json.hpp...")
#   FetchContent_PopulateFast(json)
# ENDIF()
# ADD_SUBDIRECTORY(${json_SOURCE_DIR} ${json_BINARY_DIR} EXCLUDE_FROM_ALL)
