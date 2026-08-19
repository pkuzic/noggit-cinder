# This allows faster configure time when working with cmake files

# !!! deprecated in cmake 3.11, FetchContent_MakeAvailable now handles it

macro(FetchContent_PopulateFast dep)
  set("${dep}_SOURCE_DIR" "${CMAKE_BINARY_DIR}/_deps/${dep}-src")
  set("${dep}_BINARY_DIR" "${CMAKE_BINARY_DIR}/_deps/${dep}-build")
  if(NOT ${FAST_BUILD_FETCHCONTENT} OR NOT EXISTS "${${dep}_SOURCE_DIR}")
    FetchContent_Populate(${dep})
  endif()
endmacro()