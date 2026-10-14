#include "FrameCodec.h"
#include "SearchProtocol.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments
{
    std::string host = "127.0.0.1";
    unsigned short port = 8888;
    std::size_t topK = 0;
    std::vector<std::string> queries;
};

std::size_t positiveNumber(const std::string &text, std::size_t maximum)
{
    if (text.empty()) throw std::invalid_argument("expected a positive number");
    std::size_t value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9' ||
            value > (maximum - static_cast<std::size_t>(digit - '0')) / 10) {
            throw std::invalid_argument("invalid numeric option: " + text);
        }
        value = value * 10 + static_cast<std::size_t>(digit - '0');
    }
    if (value == 0) throw std::invalid_argument("expected a positive number");
    return value;
}

Arguments parse(int argc, char *argv[])
{
    Arguments result;
    bool hostSet = false, portSet = false, topSet = false;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) throw std::invalid_argument("missing option value");
        const std::string option = argv[index];
        const std::string value = argv[index + 1];
        if (option == "--host" && !hostSet) {
            result.host = value;
            hostSet = true;
        } else if (option == "--port" && !portSet) {
            result.port = static_cast<unsigned short>(positiveNumber(value, 65535));
            portSet = true;
        } else if (option == "--top-k" && !topSet) {
            result.topK = positiveNumber(value, 20);
            topSet = true;
        } else if (option == "--query") {
            result.queries.push_back(value);
        } else {
            throw std::invalid_argument("unknown or duplicate option: " + option);
        }
    }
    if (result.queries.empty()) {
        throw std::invalid_argument("at least one --query is required");
    }
    return result;
}

void sendAll(int fd, const std::string &data)
{
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(fd, data.data() + sent,
                                     data.size() - sent, MSG_NOSIGNAL);
        if (count > 0) sent += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else throw std::runtime_error("send failed");
    }
}

int run(const Arguments &arguments)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("socket failed");
    struct SocketGuard {
        int value;
        ~SocketGuard() { ::close(value); }
    } guard{fd};
    timeval timeout{5, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(arguments.port);
    if (::inet_pton(AF_INET, arguments.host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("--host must be an IPv4 address");
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("connect failed: " +
                                 std::string(std::strerror(errno)));
    }
    std::string input;
    for (std::size_t index = 0; index < arguments.queries.size(); ++index) {
        const std::string id = "search-" + std::to_string(index + 1);
        nlohmann::json body{{"version", 1}, {"request_id", id},
                            {"query", arguments.queries[index]}};
        if (arguments.topK != 0) body["top_k"] = arguments.topK;
        sendAll(fd, TlvFrameCodec::encode(
            Frame{SearchProtocol::SearchRequestType, body.dump()}, 4096));
        while (true) {
            const FrameDecodeResult frame = TlvFrameCodec::decode(
                input, 64 * 1024, FrameDirection::ResponsesOnly);
            if (frame.status == FrameDecodeStatus::InvalidFrame) {
                throw std::runtime_error("invalid response frame");
            }
            if (frame.status == FrameDecodeStatus::FrameReady) {
                input.erase(0, frame.consumed);
                if (frame.frame.type != SearchProtocol::SearchResponseType) {
                    throw std::runtime_error("unexpected response type");
                }
                const nlohmann::json response =
                    nlohmann::json::parse(frame.frame.body);
                if (!response.is_object() || !response.contains("request_id") ||
                    response.at("request_id") != id) {
                    throw std::runtime_error("response request_id mismatch");
                }
                std::cout << response.dump() << '\n';
                break;
            }
            char bytes[8192];
            const ssize_t count = ::recv(fd, bytes, sizeof(bytes), 0);
            if (count > 0) input.append(bytes, static_cast<std::size_t>(count));
            else if (count < 0 && errno == EINTR) continue;
            else throw std::runtime_error("server closed or response timed out");
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
        return run(parse(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "search_client: " << error.what() << '\n';
        return 1;
    }
}
