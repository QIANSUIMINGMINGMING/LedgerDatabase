SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -O2 -pthread -lssl -Wall -Wno-unused-function -fPIC")

AUX_SOURCE_DIRECTORY(common common_srcs)
LIST(APPEND ledger_srcs ${common_srcs})

AUX_SOURCE_DIRECTORY(ledgerdb ledgerdb_srcs)
LIST(APPEND ledger_srcs ${ledgerdb_srcs})

AUX_SOURCE_DIRECTORY(ledgerdb/mpt mpt_srcs)
LIST(APPEND ledger_srcs ${mpt_srcs})

AUX_SOURCE_DIRECTORY(ledgerdb/skiplist skiplist_srcs)
LIST(APPEND ledger_srcs ${skiplist_srcs})

# ADD_LIBRARY(ledger_objs OBJECT ${ledger_srcs})
# ADD_LIBRARY(ledger_objs OBJECT ${ledger_srcs})
# ADD_LIBRARY(ledger STATIC $<TARGET_OBJECTS:ledger_objs>)
# ADD_LIBRARY(ledger STATIC ${ledger_srcs})
ADD_LIBRARY(ledger_objs OBJECT ${ledger_srcs})
ADD_LIBRARY(ledger STATIC $<TARGET_OBJECTS:ledger_objs>)

# find_library(CRYPTOPP_LIB cryptopp)
# find_package(TBB REQUIRED)

# LIST(APPEND LEDGER_LINKER_LIBS ${CRYPTOPP_LIB})
# LIST(APPEND LEDGER_LINKER_LIBS ${CRYPTOPP_LIB})
# LIST(APPEND LEDGER_LINKER_LIBS TBB::tbbmalloc)
# LIST(APPEND LEDGER_LINKER_LIBS TBB::tbb)
# LIST(APPEND LEDGER_LINKER_LIBS ${CMAKE_SOURCE_DIR}/ledger/gpumpt/libgmpt.a)
# LIST(APPEND LEDGER_LINKER_LIBS ${CMAKE_SOURCE_DIR}/ledger/gpumpt/libgmpt.a)
# LIST(APPEND LEDGER_LINKER_LIBS ${CMAKE_SOURCE_DIR}/ledger/gpumpt/libgmpt.a)


TARGET_LINK_LIBRARIES(ledger ${LEDGER_LINKER_LIBS})



        