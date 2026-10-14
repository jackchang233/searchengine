#include "Configuration.h"
#include "FrameCodec.h"
#include "KeywordRecommender.h"
#include "DictionaryStore.h"
#include "RecommendationProtocol.h"
#include "SearchProtocol.h"
#include "SearchEngineServer.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <spawn.h>
#include <signal.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class SocketHandle
{
public:
    explicit SocketHandle(int fd = -1) : _fd(fd) {}
    ~SocketHandle() { if (_fd >= 0) { ::close(_fd); } }
    int get() const { return _fd; }
private:
    int _fd;
};

unsigned short availablePort()
{
    SocketHandle socket(::socket(AF_INET, SOCK_STREAM, 0));
    require(socket.get() >= 0, "could not create port reservation socket");
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(socket.get(), reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0, "could not reserve port");
    socklen_t length = sizeof(address);
    require(::getsockname(socket.get(), reinterpret_cast<sockaddr *>(&address),
                          &length) == 0, "could not read reserved port");
    return ntohs(address.sin_port);
}

class RunningServer
{
public:
    explicit RunningServer(SearchEngineServer &server)
    : _server(server), _thread([this] {
        try {
            _server.start();
        } catch (...) {
            _failure = std::current_exception();
        }
    }) {}

    ~RunningServer()
    {
        if (_thread.joinable()) {
            _server.stop();
            _thread.join();
        }
    }

    void finish()
    {
        _server.stop();
        _thread.join();
        if (_failure) {
            std::rethrow_exception(_failure);
        }
    }

private:
    SearchEngineServer &_server;
    std::thread _thread;
    std::exception_ptr _failure;
};

class Client
{
public:
    explicit Client(unsigned short port)
    : _socket(::socket(AF_INET, SOCK_STREAM, 0))
    {
        require(_socket.get() >= 0, "client socket failed");
        timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        require(::setsockopt(_socket.get(), SOL_SOCKET, SO_RCVTIMEO,
                             &timeout, sizeof(timeout)) == 0,
                "client timeout failed");
        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        bool connected = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (::connect(_socket.get(), reinterpret_cast<sockaddr *>(&address),
                          sizeof(address)) == 0) {
                connected = true;
                break;
            }
            if (errno != ECONNREFUSED) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(connected, "could not connect to test server");
    }

    void sendBytes(const std::string &bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = ::send(_socket.get(), bytes.data() + offset,
                                         bytes.size() - offset, MSG_NOSIGNAL);
            require(count > 0, "test send failed");
            offset += static_cast<std::size_t>(count);
        }
    }

    void closeWrite()
    {
        require(::shutdown(_socket.get(), SHUT_WR) == 0,
                "client half-close failed");
    }

    void expectClosed()
    {
        char byte;
        require(::recv(_socket.get(), &byte, 1, 0) == 0,
                "server did not close drained connection");
    }

    nlohmann::json receive()
    {
        char buffer[4096];
        while (true) {
            const FrameDecodeResult frame = TlvFrameCodec::decode(
                _input, 64 * 1024, FrameDirection::ResponsesOnly);
            require(frame.status != FrameDecodeStatus::InvalidFrame,
                    "invalid response frame");
            if (frame.status == FrameDecodeStatus::FrameReady) {
                _input.erase(0, frame.consumed);
                require(frame.frame.type ==
                            RecommendationProtocol::RecommendResponseType ||
                            frame.frame.type == 0x80000002U,
                        "wrong response type");
                return nlohmann::json::parse(frame.frame.body);
            }
            const ssize_t count = ::recv(_socket.get(), buffer,
                                         sizeof(buffer), 0);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            require(count > 0, "response timeout or connection closed");
            _input.append(buffer, static_cast<std::size_t>(count));
        }
    }

private:
    SocketHandle _socket;
    std::string _input;
};

std::string request(const std::string &id, const std::string &query,
                    std::uint32_t type = 1)
{
    const nlohmann::json body{{"version", 1}, {"request_id", id},
                              {"query", query}, {"top_k", 2}};
    return TlvFrameCodec::encode(Frame{type, body.dump()}, 4096);
}

std::string searchRequest(const std::string &id, const std::string &query)
{
    const nlohmann::json body{{"version", 1}, {"request_id", id},
                              {"query", query}, {"top_k", 2}};
    return TlvFrameCodec::encode(
        Frame{SearchProtocol::SearchRequestType, body.dump()}, 4096);
}

ServerOptions options(unsigned short port)
{
    ServerOptions result = Configuration::load(
        SEARCHENGINE_RECOMMENDATION_FIXTURE_CONFIG,
        {{"server_port", std::to_string(port)}});
    result.threadCount = 1;
    result.queueCapacity = 8;
    result.maxInflightPerConnection = 2;
    return result;
}

void testClientExecutable(unsigned short port)
{
    const std::string portText = std::to_string(port);
    char *arguments[] = {
        const_cast<char *>(SEARCHENGINE_TEST_CLIENT),
        const_cast<char *>("--host"), const_cast<char *>("127.0.0.1"),
        const_cast<char *>("--port"), const_cast<char *>(portText.c_str()),
        const_cast<char *>("--query"), const_cast<char *>("helo"),
        const_cast<char *>("--query"), const_cast<char *>("中囯"),
        nullptr};
    pid_t child = -1;
    require(::posix_spawn(&child, SEARCHENGINE_TEST_CLIENT, nullptr, nullptr,
                          arguments, ::environ) == 0,
            "could not start recommendation_client");
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "recommendation_client failed against online server");
}

void testService()
{
    const unsigned short port = availablePort();
    const ServerOptions config = options(port);
    SearchEngineServer server(config);
    RunningServer running(server);

    Client client(port);
    const std::string first = request("en", "helo");
    const std::string second = request("cn", "中囯");
    client.sendBytes(first.substr(0, 5));
    client.sendBytes(first.substr(5) + second);
    std::map<std::string, nlohmann::json> responses;
    for (int index = 0; index < 2; ++index) {
        const nlohmann::json body = client.receive();
        responses[body.at("request_id").get<std::string>()] = body;
    }
    require(responses.size() == 2 && responses["en"]["code"] == 0 &&
                responses["cn"]["code"] == 0,
            "pipelined recommendations failed");
    require(responses["en"]["data"][0]["word"] == "hello" &&
                responses["cn"]["data"][0]["word"] == "中华" &&
                responses["cn"]["data"][1]["word"] == "中国",
            "online result differs from fixture recommendation");

    client.sendBytes(request("bad", " ") + request("unsupported", "a", 2));
    std::map<std::string, int> errors;
    for (int index = 0; index < 2; ++index) {
        const nlohmann::json body = client.receive();
        errors[body.at("request_id").get<std::string>()] =
            body.at("code").get<int>();
    }
    require(errors["bad"] == 400 && errors["unsupported"] == 501,
            "query validation or unsupported operation error is wrong");

    client.sendBytes(request("dup", "helo") + request("dup", "help"));
    std::vector<int> duplicateCodes;
    for (int index = 0; index < 2; ++index) {
        const nlohmann::json body = client.receive();
        require(body["request_id"] == "dup", "duplicate ID was not preserved");
        duplicateCodes.push_back(body["code"].get<int>());
    }
    require((duplicateCodes[0] == 0 && duplicateCodes[1] == 409) ||
                (duplicateCodes[0] == 409 && duplicateCodes[1] == 0),
            "duplicate in-flight ID was not rejected");

    client.sendBytes(request("limit1", "helo") +
                     request("limit2", "help") +
                     request("limit3", "hello"));
    std::map<std::string, int> limitCodes;
    for (int index = 0; index < 3; ++index) {
        const nlohmann::json body = client.receive();
        limitCodes[body["request_id"].get<std::string>()] =
            body["code"].get<int>();
    }
    require(limitCodes["limit3"] == 503 && limitCodes["limit1"] == 0 &&
                limitCodes["limit2"] == 0,
            "inflight limit was not enforced before submission");

    std::string batch;
    for (int index = 0; index < 80; ++index) {
        const nlohmann::json invalid{{"version", 1},
            {"request_id", "batch" + std::to_string(index)}};
        batch += TlvFrameCodec::encode(Frame{1, invalid.dump()}, 4096);
    }
    client.sendBytes(batch);
    std::set<std::string> batchIds;
    for (int index = 0; index < 80; ++index) {
        const nlohmann::json body = client.receive();
        require(body["code"] == 400, "batch error code is wrong");
        batchIds.insert(body["request_id"].get<std::string>());
    }
    require(batchIds.size() == 80,
            "frame dispatch budget stranded or duplicated frames");
    testClientExecutable(port);
    running.finish();
}

void testQueueFull()
{
    const unsigned short port = availablePort();
    ServerOptions config = options(port);
    config.queueCapacity = 1;
    config.maxInflightPerConnection = 16;
    std::atomic<bool> blocked(true);
    const SearchEngineServer::RecommendFunction slow =
        [&blocked](const std::string &, std::size_t) {
            while (blocked.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            RecommendationResult result;
            result.candidateCount = 0;
            return result;
        };
    SearchEngineServer server(config, slow);
    RunningServer running(server);
    try {
        Client client(port);
        client.sendBytes(request("one", "a") + request("two", "a") +
                         request("three", "a") + request("four", "a"));
        const nlohmann::json first = client.receive();
        require(first["code"] == 503,
                "full queue did not reject immediately");
        blocked.store(false);
        int successes = 0;
        int busy = 1;
        for (int index = 0; index < 3; ++index) {
            const int code = client.receive()["code"].get<int>();
            successes += code == 0;
            busy += code == 503;
        }
        require(successes >= 1 && busy >= 2,
                "queue saturation responses were incorrect");
        running.finish();
    } catch (...) {
        blocked.store(false);
        throw;
    }
}

void testDisconnectAndReuse()
{
    const unsigned short port = availablePort();
    ServerOptions config = options(port);
    std::atomic<bool> blocked(true);
    const SearchEngineServer::RecommendFunction slow =
        [&blocked](const std::string &, std::size_t) {
            while (blocked.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            RecommendationResult result;
            result.candidateCount = 0;
            return result;
        };
    SearchEngineServer server(config, slow);
    RunningServer running(server);
    try {
        {
            Client first(port);
            first.sendBytes(request("old", "a"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Client second(port);
        second.sendBytes(request("new", "a"));
        blocked.store(false);
        const nlohmann::json response = second.receive();
        require(response["request_id"] == "new" && response["code"] == 0,
                "disconnected request leaked into new connection");
        running.finish();
    } catch (...) {
        blocked.store(false);
        throw;
    }
}

void testInternalError()
{
    const unsigned short port = availablePort();
    const ServerOptions config = options(port);
    const SearchEngineServer::RecommendFunction failing =
        [](const std::string &, std::size_t) -> RecommendationResult {
            throw std::runtime_error("injected recommendation failure");
        };
    SearchEngineServer server(config, failing);
    RunningServer running(server);
    Client client(port);
    client.sendBytes(request("failure", "a"));
    const nlohmann::json response = client.receive();
    require(response["request_id"] == "failure" && response["code"] == 500,
            "worker exception did not become a protocol error");
    running.finish();
}

void testHalfClose()
{
    const unsigned short port = availablePort();
    ServerOptions config = options(port);
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    const SearchEngineServer::RecommendFunction slow =
        [&started, &release](const std::string &, std::size_t) {
            started.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return RecommendationResult();
        };
    SearchEngineServer server(config, slow);
    RunningServer running(server);
    try {
        Client client(port);
        client.sendBytes(request("half", "hello"));
        client.closeWrite();
        for (int attempt = 0; attempt < 1000 && !started.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(started.load(), "half-closed request was not dispatched");
        release.store(true);
        const nlohmann::json response = client.receive();
        require(response["request_id"] == "half" && response["code"] == 0,
                "half-closed client lost its completed response");
        client.expectClosed();

        Client incomplete(port);
        incomplete.sendBytes(request("incomplete", "a").substr(0, 5));
        incomplete.closeWrite();
        incomplete.expectClosed();

        Client trailing(port);
        trailing.sendBytes(request("complete", "hello") +
                           request("tail", "a").substr(0, 5));
        trailing.closeWrite();
        const nlohmann::json completed = trailing.receive();
        require(completed["request_id"] == "complete" &&
                    completed["code"] == 0,
                "trailing partial frame discarded an earlier complete request");
        trailing.expectClosed();
        running.finish();
    } catch (...) {
        release.store(true);
        throw;
    }
}

void testGracefulStop()
{
    const unsigned short port = availablePort();
    ServerOptions config = options(port);
    config.shutdownDrainTimeoutMs = 1000;
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    const SearchEngineServer::RecommendFunction slow =
        [&started, &release](const std::string &, std::size_t) {
            started.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return RecommendationResult();
        };
    SearchEngineServer server(config, slow);
    RunningServer running(server);
    try {
        Client client(port);
        client.sendBytes(request("drain", "hello"));
        for (int attempt = 0; attempt < 1000 && !started.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(started.load(), "stop test task was not dispatched");
        const auto before = std::chrono::steady_clock::now();
        server.stop();
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                       before).count();
        require(elapsed < 200, "stop blocked the management thread");
        release.store(true);
        const nlohmann::json response = client.receive();
        require(response["request_id"] == "drain" && response["code"] == 0,
                "stop discarded an accepted task");
        running.finish();
        client.expectClosed();
    } catch (...) {
        release.store(true);
        throw;
    }
}

void testDrainTimeout()
{
    const unsigned short port = availablePort();
    ServerOptions config = options(port);
    config.shutdownDrainTimeoutMs = 100;
    std::atomic<bool> started(false);
    std::atomic<bool> release(false);
    const SearchEngineServer::RecommendFunction slow =
        [&started, &release](const std::string &, std::size_t) {
            started.store(true);
            while (!release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return RecommendationResult();
        };
    SearchEngineServer server(config, slow);
    RunningServer running(server);
    try {
        Client client(port);
        client.sendBytes(request("timeout", "hello"));
        for (int attempt = 0; attempt < 1000 && !started.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(started.load(), "timeout test task was not dispatched");
        server.stop();
        client.expectClosed();
        release.store(true);
        running.finish();
    } catch (...) {
        release.store(true);
        throw;
    }
}

void testSignalStop()
{
    const unsigned short port = availablePort();
    const std::string portText = std::to_string(port);
    char *arguments[] = {
        const_cast<char *>(SEARCHENGINE_TEST_SERVER),
        const_cast<char *>("--config"),
        const_cast<char *>(SEARCHENGINE_RECOMMENDATION_FIXTURE_CONFIG),
        const_cast<char *>("--host"), const_cast<char *>("127.0.0.1"),
        const_cast<char *>("--port"), const_cast<char *>(portText.c_str()),
        nullptr};
    pid_t child = -1;
    require(::posix_spawn(&child, SEARCHENGINE_TEST_SERVER, nullptr, nullptr,
                          arguments, ::environ) == 0,
            "could not start searchengine executable");
    try {
        Client client(port);
        require(::kill(child, SIGTERM) == 0, "could not signal searchengine");
        int status = 0;
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (::waitpid(child, &status, WNOHANG) == child) {
                child = -1;
                require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                        "SIGTERM did not stop searchengine cleanly");
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("SIGTERM shutdown timed out");
    } catch (...) {
        if (child > 0) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
        }
        throw;
    }
}

void testRealDictionary()
{
    const unsigned short port = availablePort();
    const ServerOptions config = Configuration::load(
        SEARCHENGINE_TEST_CONFIG,
        {{"server_ip", "127.0.0.1"},
         {"server_port", std::to_string(port)}});
    SearchEngineServer server(config);
    RunningServer running(server);
    Client client(port);
    client.sendBytes(request("real", "helo"));
    const nlohmann::json response = client.receive();

    const std::shared_ptr<const DictionaryRepository> repository =
        DictionaryStore::loadCurrent(config.keywordIndexRoot).repository;
    const KeywordRecommender recommender(repository, config.maxQueryChars,
                                         config.maxTopK);
    const RecommendationResult expected = recommender.recommend("helo", 2);
    require(response["code"] == 0 && response["request_id"] == "real" &&
                response["data"].size() == expected.items.size(),
            "real dictionary response shape differs from CLI business path");
    for (std::size_t index = 0; index < expected.items.size(); ++index) {
        require(response["data"][index]["word"] == expected.items[index].word &&
                    response["data"][index]["distance"] ==
                        expected.items[index].distance &&
                    response["data"][index]["frequency"] ==
                        expected.items[index].frequency,
                "real dictionary online ranking differs from recommender");
    }
    client.sendBytes(searchRequest("web", "中国") +
                     request("alongside", "hello"));
    std::map<std::string, nlohmann::json> mixed;
    mixed["first"] = client.receive();
    mixed["second"] = client.receive();
    const nlohmann::json &web = mixed["first"]["request_id"] == "web"
        ? mixed["first"] : mixed["second"];
    const nlohmann::json &recommendation =
        mixed["first"]["request_id"] == "alongside"
            ? mixed["first"] : mixed["second"];
    require(web["request_id"] == "web" && web["code"] == 0 &&
                web["candidate_count"].get<std::size_t>() > 0 &&
                !web["generation"].get<std::string>().empty() &&
                !web["data"].empty() &&
                web["data"][0].contains("docid") &&
                web["data"][0].contains("score") &&
                recommendation["request_id"] == "alongside" &&
                recommendation["code"] == 0,
            "web search and recommendation did not coexist");

    const std::string portText = std::to_string(port);
    char *arguments[] = {
        const_cast<char *>(SEARCHENGINE_TEST_SEARCH_CLIENT),
        const_cast<char *>("--host"), const_cast<char *>("127.0.0.1"),
        const_cast<char *>("--port"), const_cast<char *>(portText.c_str()),
        const_cast<char *>("--query"), const_cast<char *>("中国"),
        nullptr};
    pid_t child = -1;
    require(::posix_spawn(&child, SEARCHENGINE_TEST_SEARCH_CLIENT,
                          nullptr, nullptr, arguments, ::environ) == 0,
            "could not start search_client");
    int status = 0;
    require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "search_client failed against online server");
    running.finish();
}

} // namespace

int main()
{
    try {
        testService();
        testQueueFull();
        testDisconnectAndReuse();
        testInternalError();
        testHalfClose();
        testGracefulStop();
        testDrainTimeout();
        testSignalStop();
        testRealDictionary();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "SearchEngineServerTest failed: " << error.what() << '\n';
        return 1;
    }
}
