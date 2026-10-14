#include "EchoServer.h"
#include <functional>
#include <iostream>
#include <utility>

MyTask::MyTask(std::string message, TcpConnectionPtr connection)
: _message(std::move(message))
, _connection(std::move(connection))
{
}

void MyTask::process()
{
    // 耗时业务在工作线程执行；这里只用 echo 作为示例。
    std::string response = "echo: " + _message;

    // 工作线程只投递发送任务，实际 socket 写操作由 Reactor 执行。
    _connection->sendInLoop(response);
}

EchoServer::EchoServer(size_t threadNum,
                       size_t queueSize,
                       const std::string &ip,
                       unsigned short port)
: _pool(threadNum, queueSize)
, _server(ip, port)
{
}

EchoServer::~EchoServer()
{
    stop();
    _pool.stop();
}

void EchoServer::start()
{
    _pool.start();

    using namespace std::placeholders;
    _server.setAllCallback(
        std::bind(&EchoServer::onConnection, this, _1),
        std::bind(&EchoServer::onFrame, this, _1, _2),
        std::bind(&EchoServer::onClose, this, _1));

    try {
        _server.start();
    } catch (...) {
        _pool.stop();
        throw;
    }

    _pool.stop();
}

void EchoServer::stop()
{
    _server.stop();
}

void EchoServer::onConnection(const TcpConnectionPtr &connection)
{
    std::cout << "[new conn] " << connection->toString();
}

void EchoServer::onFrame(const TcpConnectionPtr &connection, const Frame &frame)
{
    // 连接层已消费一帧；业务回调不再读取输入缓冲。
    const std::string &message = frame.body;
    if (message.empty()) {
        return;
    }

    std::cout << "<<< " << message;
    bool accepted = _pool.addTask(
        std::bind(&MyTask::process, MyTask(message, connection)));

    if (!accepted) {
        connection->sendInLoop("server is stopping\n");
    }
}

void EchoServer::onClose(const TcpConnectionPtr &connection)
{
    (void)connection;
    std::cout << "[closed]" << std::endl;
}
