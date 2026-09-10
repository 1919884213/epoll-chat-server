cmake_minimum_required (VERSION 3.10)

project(pro1)

add_executable(Pro
    main.c
    Server/Server.c
    Client/Client.c
    pool/pool.c
)
add_executable(Client
    Client/Client.c
    main.h
)

add_executable(pool
    pool/pool.c
    pool/pool.h
)

target_include_directories(Pro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Client
    ${CMAKE_CURRENT_SOURCE_DIR}/pool
)