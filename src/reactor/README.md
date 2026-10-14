# Reactor 模块

`src/reactor/` 根目录只放正式服务使用的网络和线程池实现：`Acceptor`、`EventLoop`、`TcpConnection`、`TcpServer`、`TaskQueue`、`ThreadPool` 等。`tests/` 存放对应的回归测试；`examples/echo/` 保留独立 Echo 演示。正式搜索服务入口位于 `src/online/Main.cpp`，不是 Echo 示例。

CMake 目标分别为 `searchengine_net`、`searchengine_echo`、`echo_server_example` 及三个网络层测试。历史 `reactorv3/Model`、`v5` 路径已迁移；新增网络代码请放在本目录，测试放在 `tests/`。
