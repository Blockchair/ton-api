cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

find_path(
    UNI_INCLUDE_DIR
    NAMES univalue.h
    DOC "univalue include dir"
)

set(CMAKE_CXX_STANDARD 14)

set(UNI_INCLUDE_DIRS ${UNI_INCLUDE_DIR})

find_package(MHD)
add_cxx_compiler_flag("-lpq")
add_cxx_compiler_flag("-lpqxx")

if (MHD_FOUND)

  set(BLOCHAIN_API_SOURCE 
    blockchain-api.cpp
    blockchain-api.hpp
    blockchain-api-http.cpp
    blockchain-api-http.hpp
    blockchain-api-query.cpp
    blockchain-api-query.hpp
    query-impl/block-info.cpp     
    query-impl/block-search.cpp
    query-impl/run-method.cpp     
    query-impl/view-account.cpp     
    query-impl/view-transaction-extended.cpp
    query-impl/block-search-extended.cpp  
    query-impl/block-search-hash.cpp  
    query-impl/config.cpp              
    query-impl/search-by-height.cpp  
    query-impl/validators.cpp  
    query-impl/view-last-block.cpp  
    query-impl/view-transaction.cpp
    )

  add_executable(blockchain-api ${BLOCHAIN_API_SOURCE})
  target_include_directories(blockchain-api PUBLIC ${MHD_INCLUDE_DIRS} ${UNI_INCLUDE_DIR}, "/usr/include/postgresql/")
  target_link_libraries(blockchain-api tdutils tdactor adnllite tl_lite_api tl-lite-utils univalue
    ton_crypto ton_block ${MHD_LIBRARY})

    install(TARGETS blockchain-api RUNTIME DESTINATION bin)

endif()
