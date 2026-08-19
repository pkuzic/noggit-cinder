# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# Dependency: lodepng
FetchContent_Declare (lodepng
  GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
  GIT_TAG 7fdcc96a5e5864eee72911c3ca79b1d9f0d12292
)
FetchContent_MakeAvailable(lodepng) # replaces FetchContent_PopulateFast

# FetchContent_GetProperties (lodepng)
# IF(NOT lodepng_POPULATED)
#   MESSAGE(STATUS "Installing lodepng...")
#   FetchContent_PopulateFast(lodepng)
# ENDIF()
ADD_LIBRARY(lodepng "${lodepng_SOURCE_DIR}/lodepng.cpp")
TARGET_INCLUDE_DIRECTORIES(lodepng SYSTEM PUBLIC ${lodepng_SOURCE_DIR})