#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct SocketPair
{
    int descriptors[2] = {-1, -1};

    SocketPair()
    {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            throw std::runtime_error("socketpair failed");
        }
    }

    ~SocketPair()
    {
        for (int &descriptor : descriptors) {
            if (descriptor >= 0) {
                ::close(descriptor);
            }
        }
    }

    void send(const std::string &bytes)
    {
        const ssize_t written = ::send(descriptors[1], bytes.data(),
                                       bytes.size(), 0);
        require(written == static_cast<ssize_t>(bytes.size()),
                "test fixture failed to write bytes");
    }
};

void testLineFrames(EventLoop &loop)
{
    SocketPair sockets;
    std::vector<std::string> delivered;
    TcpConnPtr connection = std::make_shared<TcpConnection>(
        sockets.descriptors[0], loop,
        [&](const TcpConnPtr &conn, const Frame &frame) {
            delivered.push_back(frame.body);
            conn->send("echo: " + frame.body);
        });
    sockets.descriptors[0] = -1;

    sockets.send("first\nsecond");
    require(connection->handleReadEvent() && delivered.size() == 1 &&
                delivered[0] == "first\n",
            "line frame was not delivered exactly once");
    char response[64];
    const ssize_t count = ::recv(sockets.descriptors[1], response,
                                 sizeof(response), MSG_DONTWAIT);
    require(count == 12 && std::string(response, count) == "echo: first\n",
            "Echo line response bytes changed");
    sockets.send("\n");
    require(connection->handleReadEvent() && delivered.size() == 2 &&
                delivered[1] == "second\n",
            "split line was not delivered after its terminator");
}

void testTlvFrames(EventLoop &loop)
{
    SocketPair sockets;
    std::vector<Frame> delivered;
    TcpConnPtr connection = std::make_shared<TcpConnection>(
        sockets.descriptors[0], loop,
        [&](const TcpConnPtr &, const Frame &frame) {
            delivered.push_back(frame);
        }, nullptr, FrameMode::Tlv, 32);
    sockets.descriptors[0] = -1;

    const std::string first = TlvFrameCodec::encode(Frame{1, "{\n}"}, 32);
    const std::string second = TlvFrameCodec::encode(Frame{2, "{}"}, 32);
    sockets.send(first.substr(0, 5));
    require(connection->handleReadEvent() && delivered.empty(),
            "partial TLV header was dispatched");
    sockets.send(first.substr(5) + second);
    require(connection->handleReadEvent() && delivered.size() == 2 &&
                delivered[0].body == "{\n}" && delivered[1].type == 2,
            "TLV framing misread newline or coalesced frames");

    const std::string overlong = TlvFrameCodec::encode(Frame{1, std::string(33, 'a')}, 33);
    sockets.send(overlong.substr(0, 8));
    require(!connection->handleReadEvent(),
            "oversized TLV body was not rejected from its header");
}

void testResponseDirection(EventLoop &loop)
{
    SocketPair sockets;
    TcpConnPtr connection = std::make_shared<TcpConnection>(
        sockets.descriptors[0], loop, nullptr, nullptr, FrameMode::Tlv, 32);
    sockets.descriptors[0] = -1;
    sockets.send(TlvFrameCodec::encode(Frame{0x80000001U, "{}"}, 32));
    require(!connection->handleReadEvent(),
            "client response frame was not rejected");
}

void testSlowReaderFlush(EventLoop &loop)
{
    SocketPair sockets;
    const int smallBuffer = 4096;
    require(::setsockopt(sockets.descriptors[0], SOL_SOCKET, SO_SNDBUF,
                         &smallBuffer, sizeof(smallBuffer)) == 0,
            "could not constrain server send buffer");
    TcpConnPtr connection = std::make_shared<TcpConnection>(
        sockets.descriptors[0], loop);
    sockets.descriptors[0] = -1;
    const std::string payload(256 * 1024, 'x');
    connection->send(payload);
    require(connection->hasPendingOutput(),
            "slow reader did not create pending output");
    require(::shutdown(sockets.descriptors[1], SHUT_WR) == 0 &&
                connection->handleReadEvent() && connection->isReadClosed(),
            "half-close discarded pending output");
    connection->closeAfterFlush();

    std::size_t received = 0;
    bool writable = true;
    for (int attempt = 0; attempt < 1000 && writable; ++attempt) {
        char bytes[8192];
        while (true) {
            const ssize_t count = ::recv(sockets.descriptors[1], bytes,
                                         sizeof(bytes), MSG_DONTWAIT);
            if (count > 0) {
                received += static_cast<std::size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                require(count < 0 &&
                            (errno == EAGAIN || errno == EWOULDBLOCK),
                        "unexpected slow reader receive result");
                break;
            }
        }
        writable = connection->handleWriteEvent();
    }
    require(!writable && !connection->hasPendingOutput(),
            "close-after-flush did not wait for queued output");
    char bytes[8192];
    ssize_t count;
    while ((count = ::recv(sockets.descriptors[1], bytes, sizeof(bytes),
                           MSG_DONTWAIT)) > 0) {
        received += static_cast<std::size_t>(count);
    }
    require(received == payload.size(),
            "slow reader lost buffered response bytes");
}

} // namespace

int main()
{
    try {
        Acceptor acceptor("127.0.0.1", 0);
        EventLoop loop(acceptor);
        testLineFrames(loop);
        testTlvFrames(loop);
        testResponseDirection(loop);
        testSlowReaderFlush(loop);
    } catch (const std::exception &error) {
        std::cerr << "TcpConnectionFramingTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
