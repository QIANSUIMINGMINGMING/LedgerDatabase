CMAKE_MINIMUM_REQUIRED(VERSION 3.20)

set(CMAKE_CUDA_ARCHITECTURES 60 61 62 70 72 75 86)
set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
set(CMAKE_CUDA_HOST_COMPILER g++-10)
set(CMAKE_CXX_COMPILER g++-10)
set(CMAKE_C_COMPILER gcc-10)

PROJECT(ledgebase LANGUAGES CXX CUDA)

LIST(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

OPTION(LEDGERDB "Use LedgerDB" ON)

if(LEDGERDB)
  message(STATUS "Use LedgerDB")
  add_definitions(-DLEDGERDB)
endif()

SET(BOOST_COMPONENT "filesystem")
LIST(APPEND BOOST_COMPONENT "program_options")
LIST(APPEND BOOST_COMPONENT "thread")

# ---[ Dependencies
include(cmake/Dependencies.cmake)

enable_testing()
# SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -O2 -pthread -lssl -Wall -Wno-unused-function -fPIC -c")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -O2 -pthread -lssl -Wall -Wno-unused-function -fPIC")

MESSAGE(STATUS "${PROTOBUF_PROTOC_EXECUTABLE}")
LIST(APPEND LEDGEBASE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}")
LIST(APPEND LEDGEBASE_INCLUDE_DIR "${CMAKE_BINARY_DIR}")
INCLUDE_DIRECTORIES(${LEDGEBASE_INCLUDE_DIR})
MESSAGE(STATUS "include: ${LEDGEBASE_INCLUDE_DIR}")


SET(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)
SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

ADD_SUBDIRECTORY(ledger)
ADD_SUBDIRECTORY(distributed)
ADD_SUBDIRECTORY(test)

SET(NONEXE_PERMISSION
    OWNER_READ
    OWNER_WRITE
    GROUP_READ
    WORLD_READ)

SET(EXE_PERMISSION
    ${NONEXE_PERMISSION}
    OWNER_EXECUTE
    GROUP_EXECUTE
    WORLD_EXECUTE)

INSTALL(DIRECTORY ${LIBRARY_OUTPUT_PATH} DESTINATION .
        FILE_PERMISSIONS ${EXE_PERMISSION}
        FILES_MATCHING PATTERN "*.so")
INSTALL(DIRECTORY ${LIBRARY_OUTPUT_PATH} DESTINATION .
        FILE_PERMISSIONS ${NONEXE_PERMISSION}
        FILES_MATCHING PATTERN "*.a")
INSTALL(DIRECTORY ${EXECUTABLE_OUTPUT_PATH} DESTINATION .
        FILE_PERMISSIONS ${EXE_PERMISSION})
INSTALL(DIRECTORY ${CMAKE_SOURCE_DIR}/distributed
                  ${CMAKE_SOURCE_DIR}/ledger
        DESTINATION include/LedgerDatabase
        FILES_MATCHING PATTERN "*.h*")
INSTALL(DIRECTORY ${CMAKE_BINARY_DIR}/distributed/proto
        DESTINATION include/LedgerDatabase/distributed)
