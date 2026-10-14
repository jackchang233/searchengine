#ifndef V5_ECHOSERVER_H
#define V5_ECHOSERVER_H

#include "ThreadPool.h"
#include "TcpServer.h"
#include <cstddef>
#include <string>

class MyTask
{
public:
    MyTask(std::string message, TcpConnectionPtr connection);

    void process();

private:
    std::string _message;
    TcpConnectionPtr _connection;
};

class EchoServer
{
public:
    EchoServer(size_t threadNum,
               size_t queueSize,
               const std::string &ip,
               unsigned short port);
    ~EchoServer();

    void start();
    void stop();

private:
    void onConnection(const TcpConnectionPtr &connection);
    void onFrame(const TcpConnectionPtr &connection, const Frame &frame);
    void onClose(const TcpConnectionPtr &connection);

private:
    ThreadPool _pool;
    TcpServer _server;
};

#endif
