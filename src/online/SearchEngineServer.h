#pragma once

#include "Configuration.h"
#include "KeywordRecommender.h"
#include "RecommendationProtocol.h"
#include "SearchProtocol.h"
#include "TcpServer.h"
#include "ThreadPool.h"
#include "webpage/WebSearcher.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

class SearchEngineServer
{
public:
    using RecommendFunction = std::function<RecommendationResult(
        const std::string &, std::size_t)>;

    explicit SearchEngineServer(const ServerOptions &options,
                                RecommendFunction recommend = RecommendFunction());
    ~SearchEngineServer();

    // 在调用线程运行 Reactor，直到 stop() 被调用。
    void start();
    void stop();

private:
    struct Session
    {
        bool active = true;
        std::set<std::string> inflight;
    };

    void onConnection(const TcpConnPtr &connection);
    void onFrame(const TcpConnPtr &connection, const Frame &frame);
    void onClose(const TcpConnPtr &connection);
    void onReadClosed(const TcpConnPtr &connection);
    bool workDrained() const;
    void sendError(const TcpConnPtr &connection, std::uint32_t type,
                   const ProtocolError &error);

    ServerOptions _options;
    ProtocolLimits _limits;
    std::shared_ptr<const DictionaryRepository> _repository;
    std::shared_ptr<const KeywordRecommender> _recommender;
    RecommendFunction _recommend;
    std::shared_ptr<const WebIndexSnapshot> _webSnapshot;
    std::shared_ptr<const WebSearcher> _webSearcher;
    ThreadPool _pool;
    TcpServer _server;
    std::map<const TcpConnection *, std::shared_ptr<Session>> _sessions;
    std::atomic<bool> _accepting;
    std::atomic<bool> _stopping;
};
