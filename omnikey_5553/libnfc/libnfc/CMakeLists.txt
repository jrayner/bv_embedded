# Library
SET(LIBRARY_SOURCES nfc iso14443-subr)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})
ADD_LIBRARY(nfc SHARED ${LIBRARY_SOURCES})
TARGET_LINK_LIBRARIES(nfc)
SET_TARGET_PROPERTIES(nfc PROPERTIES SOVERSION 0)

IF(MSVC)
  # On Windows the shared (runtime) library should be either in the same 
  # directory as the excutables or in the path, we add it to same directory
  INSTALL(TARGETS nfc RUNTIME DESTINATION bin COMPONENT libraries)

  # At compile time we need the .LIB file, we place it in the lib directory
  INSTALL(TARGETS nfc ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT headers)
ELSE(MSVC)
  INSTALL(TARGETS nfc LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT libraries)
ENDIF(MSVC)

