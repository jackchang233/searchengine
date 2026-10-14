#include "FrameCodec.h"
#include "RecommendationProtocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments
{
    std::string host = "127.0.0.1";
    unsigned short port = 8888;
    std::vector<std::string> queries;
    std::size_t topK = 0;
};

std::size_t positiveNumber(const std::string &text, std::size_t maximum)
{
    if (text.empty()) {
        throw std::invalid_argument("expected a positive number");
    }
    std::size_t result = 0;
    for (char character : text) {
        if (character < '0' || character > '9' ||
            result > (maximum - static_cast<std::size_t>(character - '0')) / 10) {
            throw std::invalid_argument("invalid number: " + text);
        }
        result = result * 10 + static_cast<std::size_t>(character - '0');
    }
    if (result == 0) {
        throw std::invalid_argument("expected a positive number");
    }
    return result;
}

Arguments parseArguments(int argc, char *argv[])
{
    Arguments result;
    bool hasHost = false;
    bool hasPort = false;
    bool hasTopK = false;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing option value");
        }
        const std::string option = argv[index];
        const std::string value = argv[index + 1];
        if (option == "--host" && !hasHost) {
            result.host = value;
            hasHost = true;
        } else if (option == "--port" && !hasPort) {
            result.port = static_cast<unsigned short>(
                positiveNumber(value, 65535));
            hasPort = true;
        } else if (option == "--query") {
            result.queries.push_back(value);
        } else if (option == "--top-k" && !hasTopK) {
            result.topK = positiveNumber(value, 20);
            hasTopK = true;
        } else {
            throw std::invalid_argument("unknown or duplicate option: " +
                                        option);
        }
    }
    if (result.queries.empty()) {
        throw std::invalid_argument("at least one --query is required");
    }
    return result;
}

class SocketHandle
{
public:
    explicit SocketHandle(int fd) : _fd(fd) {}
    ~SocketHandle() { if (_fd >= 0) { ::close(_fd); } }
    int get() const { return _fd; }
private:
    int _fd;
};

void sendAll(int fd, const std::string &data)
{
    std::size_t position = 0;
    while (position < data.size()) {
        const ssize_t count = ::send(fd, data.data() + position,
                                     data.size() - position, MSG_NOSIGNAL);
        if (count > 0) {
            position += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error("send failed: " +
                                     std::string(std::strerror(errno)));
        }
    }
}

int run(const Arguments &arguments)
{
    SocketHandle socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (socket.get() < 0) {
        throw std::runtime_error("socket failed");
    }
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    ::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout));
    ::setsockopt(socket.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(arguments.port);
    if (::inet_pton(AF_INET, arguments.host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("--host must be an IPv4 address");
    }
    if (::connect(socket.get(), reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) != 0) {
        throw std::runtime_error("connect failed: " +
                                 std::string(std::strerror(errno)));
    }

    std::set<std::string> outstanding;
    for (std::size_t index = 0; index < arguments.queries.size(); ++index) {
        const std::string id = "req-" + std::to_string(index + 1);
        nlohmann::json body{{"version", 1}, {"request_id", id},
                            {"query", arguments.queries[index]}};
        if (arguments.topK != 0) {
            body["top_k"] = arguments.topK;
        }
        sendAll(socket.get(), TlvFrameCodec::encode(
            Frame{RecommendationProtocol::RecommendRequestType, body.dump()},
            16 * 1024 * 1024));
        outstanding.insert(id);
    }

    std::string input;
    char buffer[8192];
    while (!outstanding.empty()) {
        const FrameDecodeResult frame = TlvFrameCodec::decode(
            input, 64 * 1024, FrameDirection::ResponsesOnly);
        if (frame.status == FrameDecodeStatus::InvalidFrame) {
            throw std::runtime_error("invalid response frame");
        }
        if (frame.status == FrameDecodeStatus::FrameReady) {
            input.erase(0, frame.consumed);
            if (frame.frame.type !=
                RecommendationProtocol::RecommendResponseType) {
                throw std::runtime_error("unexpected response type");
            }
            const nlohmann::json body = nlohmann::json::parse(frame.frame.body);
            if (!body.is_object() || !body.contains("request_id") ||
                !body["request_id"].is_string()) {
                throw std::runtime_error("response has no request_id");
            }
            const std::string id = body["request_id"].get<std::string>();
            if (outstanding.erase(id) != 1) {
                throw std::runtime_error("unknown or duplicate response ID: " + id);
            }
            std::cout << body.dump() << '\n';
            continue;
        }
        const ssize_t count = ::recv(socket.get(), buffer, sizeof(buffer), 0);
        if (count > 0) {
            input.append(buffer, static_cast<std::size_t>(count));
        } else if (count == 0) {
            throw std::runtime_error("server closed before all responses");
        } else if (errno != EINTR) {
            throw std::runtime_error("recv failed: " +
                                     std::string(std::strerror(errno)));
        }
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "usage: " << argv[0]
                  << " [--host IPv4] [--port NUMBER] --query TEXT"
                     " [--query TEXT ...] [--top-k NUMBER]\n";
        return 0;
    }
    try {
        return run(parseArguments(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "recommendation_client: " << error.what() << '\n';
        return 1;
    }
}
