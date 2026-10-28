cmake_minimum_required (VERSION 3.10)

project(pro1 C)   # 纯 C 项目：只启用 C，免掉交叉编译时对 g++ 的依赖

# 服务器：epoll 事件循环 + 线程池 + 聊天核心 + TCP 初始化
add_executable(Pro
    main.c
    Server/Server.c
    Server/chatsnap_hook.c
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

# 内核快照监控端：验证 /dev/chatsnap 的 poll/read/ioctl
add_executable(ChatSnapMon
    tools/chatsnap_mon.c
)

target_include_directories(Client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Client
)

target_include_directories(ChatSnapMon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/kernel/chatsnap
)

find_package(Threads REQUIRED)
target_link_libraries(Pro PRIVATE Threads::Threads)
target_link_libraries(Client PRIVATE Threads::Threads)
