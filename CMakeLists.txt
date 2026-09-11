cmake_minimum_required (VERSION 3.10)

project(pro1)

# 服务器：epoll 事件循环 + 线程池 + 聊天核心 + TCP 初始化
add_executable(Pro
    main.c
    Server/Server.c
    Chat/Chat.c
    pool/pool.c
)

# 交互式聊天客户端
add_executable(Client
    Client/Client.c
)

target_include_directories(Pro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Server
    ${CMAKE_CURRENT_SOURCE_DIR}/Chat
    ${CMAKE_CURRENT_SOURCE_DIR}/pool
)

target_include_directories(Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Client
)

find_package(Threads REQUIRED)
target_link_libraries(Pro PRIVATE Threads::Threads)
target_link_libraries(Client PRIVATE Threads::Threads)
