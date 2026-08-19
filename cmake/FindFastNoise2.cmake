# This file is part of Noggit3, licensed under GNU General Public License (version 3).

# Dependency: FastNoise2
FetchContent_Declare (fastnoise2
        GIT_REPOSITORY https://github.com/tswow/FastNoise2.git
        GIT_TAG v0.0.1-heightmap
        PATCH_COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_SOURCE_DIR}/cmake/deps/patch_fastnoise2.cmake"
        UPDATE_DISCONNECTED true
        )

FetchContent_MakeAvailable(fastnoise2) # replaces FetchContent_PopulateFast

# FetchContent_GetProperties (fastnoise2)
# IF(NOT fastnoise2_POPULATED)
#   MESSAGE(STATUS "Installing FastNoise2...")
#   FetchContent_PopulateFast(fastnoise2)
# ENDIF()

SET(FASTNOISE2_NOISETOOL:BOOL OFF)
SET(FASTNOISE2_TESTS:BOOL OFF)

IF(FASTNOISE2_NOISETOOL)
  ADD_SUBDIRECTORY(${fastnoise2_SOURCE_DIR} ${fastnoise2_BINARY_DIR})
ELSE()
  ADD_SUBDIRECTORY(${fastnoise2_SOURCE_DIR} ${fastnoise2_BINARY_DIR} EXCLUDE_FROM_ALL)
ENDIF()