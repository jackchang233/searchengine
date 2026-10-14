#include "SearchEngineServer.h"

#include "DictionaryStore.h"
#include "QueryNormalizer.h"

#include <exception>
#include <iostream>
#include <utility>
#include <vector>

namespace {

const std::size_t kMaxResponseBytes = 64 * 1024;

std::shared_ptr<const DictionaryRepository> loadRepository(
    const ServerOptions &options)
{
    if (!options.keywordIndexRoot.empty()) {
        return DictionaryStore::loadCurrent(options.keywordIndexRoot).repository;
    }
    return DictionaryRepository::load({
        {"en", options.enDictionaryPath, options.enIndexPath},
        {"cn", options.cnDictionaryPath, options.cnIndexPath}});
}

ProtocolLimits makeLimits(const ServerOptions &options)
{
    ProtocolLimits limits;
    limits.defaultTopK = options.defaultTopK;
    limits.maxTopK = options.maxTopK;
    limits.maxQueryChars = options.maxQueryChars;
    limits.maxBodyBytes = options.maxBodyBytes;
    limits.maxJsonDepth = options.maxJsonDepth;
    return limits;
}

std::string encodeFailure(const std::string &requestId, int code,
                          const std::string &message)
{
    ProtocolResponse response;
    response.hasRequestId = true;
    response.requestId = requestId;
    response.code = code;
    response.message = message;
    return RecommendationProtocol::encodeResponse(response, kMaxResponseBytes);
}

std::string computeResponse(
    const RecommendationRequest &request,
    const SearchEngineServer::RecommendFunction &recommend)
{
    try {
        const RecommendationResult result =
            recommend(request.query, request.topK);
        ProtocolResponse response;
        response.hasRequestId = true;
        response.requestId = request.requestId;
        for (const Recommendation &item : result.items) {
            response.data.push_back(ProtocolRecommendation{
                item.word, item.distance, item.frequency});
        }
        return RecommendationProtocol::encodeResponse(response,
                                                       kMaxResponseBytes);
    } catch (const QueryValidationError &error) {
        return encodeFailure(request.requestId, 400, error.what());
    } catch (const std::exception &error) {
        std::cerr << "recommendation failed: " << error.what() << '\n';
        return encodeFailure(request.requestId, 500, "internal error");
    }
}

std::string computeSearchResponse(
    const SearchRequest &request,
    const std::shared_ptr<const WebSearcher> &searcher)
{
    try {
        const WebSearchResult result = searcher->search(request.query, request.topK);
        SearchResponse response;
        response.hasRequestId = true;
        response.requestId = request.requestId;
        response.generation = result.generation;
        response.candidateCount = result.candidateCount;
        for (const WebSearchHit &hit : result.hits) {
            response.data.push_back(ProtocolSearchHit{
                hit.documentId, hit.title, hit.url, hit.snippet, hit.score});
        }
        return SearchProtocol::encodeResponse(response, kMaxResponseBytes);
    } catch (const std::invalid_argument &error) {
        SearchResponse failure = SearchProtocol::errorResponse(
            ProtocolError(400, error.what(), request.requestId));
        return SearchProtocol::encodeResponse(failure, kMaxResponseBytes);
    } catch (const std::exception &error) {
        std::cerr << "web search failed: " << error.what() << '\n';
        SearchResponse failure = SearchProtocol::errorResponse(
            ProtocolError(500, "internal error", request.requestId));
        return SearchProtocol::encodeResponse(failure, kMaxResponseBytes);
    }
}

std::shared_ptr<const WebIndexSnapshot> loadWebSnapshot(
    const ServerOptions &options)
{
    if (options.webIndexRoot.empty()) {
        return {};
    }
    std::shared_ptr<WebIndexSnapshot> snapshot =
        std::make_shared<WebIndexSnapshot>(
            WebIndexStore::loadCurrent(options.webIndexRoot));
    snapshot->validateAnalyzerInputs(options.jiebaDictionaryDirectory,
                                     options.webStopWordFiles);
    return snapshot;
}

} // namespace

SearchEngineServer::SearchEngineServer(const ServerOptions &options,
                                       RecommendFunction recommend)
: _options(options)
, _limits(makeLimits(options))
, _repository(loadRepository(options))
, _recommender(std::make_shared<KeywordRecommender>(
      _repository, options.maxQueryChars, options.maxTopK))
, _recommend(std::move(recommend))
, _webSnapshot(loadWebSnapshot(options))
, _webSearcher(_webSnapshot ? std::make_shared<WebSearcher>(
      _webSnapshot, options.jiebaDictionaryDirectory,
      options.webStopWordFiles, options.maxQueryChars, options.maxTopK) :
      std::shared_ptr<WebSearcher>())
, _pool(options.threadCount, options.queueCapacity)
, _server(options.serverIp, options.serverPort, FrameMode::Tlv,
          options.maxBodyBytes)
, _sessions()
, _accepting(false)
, _stopping(false)
{
    if (!_recommend) {
        const std::shared_ptr<const KeywordRecommender> recommender =
            _recommender;
        _recommend = [recommender](const std::string &query,
                                   std::size_t topK) {
            return recommender->recommend(query, topK);
        };
    }
}

SearchEngineServer::~SearchEngineServer()
{
    stop();
    _pool.stop();
}

void SearchEngineServer::start()
{
    _pool.start();
    _server.setAllCallback(
        [this](const TcpConnPtr &connection) { onConnection(connection); },
        [this](const TcpConnPtr &connection, const Frame &frame) {
            onFrame(connection, frame);
        },
        [this](const TcpConnPtr &connection) { onClose(connection); },
        [this](const TcpConnPtr &connection) { onReadClosed(connection); });
    _accepting.store(!_stopping.load());
    try {
        _server.start();
    } catch (...) {
        _accepting.store(false);
        _pool.stop();
        throw;
    }
    _accepting.store(false);
    _pool.stop();
}

void SearchEngineServer::stop()
{
    if (_stopping.exchange(true)) {
        return;
    }
    _accepting.store(false);
    _server.beginDrain(_options.shutdownDrainTimeoutMs,
                       [this] { return workDrained(); });
}

bool SearchEngineServer::workDrained() const
{
    for (const auto &entry : _sessions) {
        if (!entry.second->inflight.empty()) {
            return false;
        }
    }
    return true;
}

void SearchEngineServer::onConnection(const TcpConnPtr &connection)
{
    _sessions.emplace(connection.get(), std::make_shared<Session>());
}

void SearchEngineServer::sendError(const TcpConnPtr &connection,
                                   std::uint32_t type,
                                   const ProtocolError &error)
{
    if (type == SearchProtocol::SearchRequestType) {
        connection->send(SearchProtocol::encodeResponse(
            SearchProtocol::errorResponse(error), kMaxResponseBytes));
        return;
    }
    const ProtocolResponse response =
        RecommendationProtocol::errorResponse(type, error);
    connection->send(RecommendationProtocol::encodeResponse(response,
                                                             kMaxResponseBytes));
}

void SearchEngineServer::onFrame(const TcpConnPtr &connection,
                                 const Frame &frame)
{
    std::string requestId;
    std::function<std::string()> work;
    try {
        if (frame.type == SearchProtocol::SearchRequestType) {
            const SearchRequest request =
                SearchProtocol::decodeRequest(frame, _limits);
            requestId = request.requestId;
            if (!_webSearcher) {
                throw ProtocolError(501, "web search is not configured", requestId);
            }
            const std::shared_ptr<const WebSearcher> searcher = _webSearcher;
            work = [request, searcher] {
                return computeSearchResponse(request, searcher);
            };
        } else {
            const RecommendationRequest request =
                RecommendationProtocol::decodeRequest(frame, _limits);
            requestId = request.requestId;
            const RecommendFunction recommend = _recommend;
            work = [request, recommend] {
                return computeResponse(request, recommend);
            };
        }
    } catch (const ProtocolError &error) {
        sendError(connection, frame.type, error);
        return;
    }

    if (!_accepting.load()) {
        sendError(connection, frame.type,
                  ProtocolError(504, "server is stopping", requestId));
        return;
    }

    const auto found = _sessions.find(connection.get());
    if (found == _sessions.end()) {
        return;
    }
    const std::shared_ptr<Session> session = found->second;
    if (session->inflight.count(requestId) != 0) {
        sendError(connection, frame.type,
                  ProtocolError(409, "duplicate request_id", requestId));
        return;
    }
    if (session->inflight.size() >= _options.maxInflightPerConnection) {
        sendError(connection, frame.type,
                  ProtocolError(503, "too many inflight requests",
                                requestId));
        return;
    }

    session->inflight.insert(requestId);
    const std::weak_ptr<TcpConnection> weakConnection = connection;
    const std::weak_ptr<Session> weakSession = session;
    const SubmitResult submitted = _pool.tryAddTask(
        [weakConnection, weakSession, work, requestId] {
            const std::string encoded = work();
            const TcpConnPtr conn = weakConnection.lock();
            if (!conn) {
                return;
            }
            conn->postToLoop([weakSession, requestId, encoded](
                                 const TcpConnPtr &completed) {
                const std::shared_ptr<Session> state = weakSession.lock();
                if (!state || !state->active || !completed->isConnected()) {
                    return;
                }
                if (state->inflight.erase(requestId) == 1) {
                    completed->send(encoded);
                    if (state->inflight.empty() && completed->isReadClosed()) {
                        completed->closeAfterFlush();
                    }
                }
            });
        });
    if (submitted != SubmitResult::Accepted) {
        session->inflight.erase(requestId);
        const int code = submitted == SubmitResult::Stopped ? 504 :
                         submitted == SubmitResult::QueueFull ? 503 : 500;
        sendError(connection, frame.type,
                  ProtocolError(code, code == 503 ? "server busy" :
                                      code == 504 ? "server is stopping" :
                                                    "invalid task",
                                requestId));
    }
}

void SearchEngineServer::onClose(const TcpConnPtr &connection)
{
    const auto found = _sessions.find(connection.get());
    if (found != _sessions.end()) {
        found->second->active = false;
        found->second->inflight.clear();
        _sessions.erase(found);
    }
}

void SearchEngineServer::onReadClosed(const TcpConnPtr &connection)
{
    const auto found = _sessions.find(connection.get());
    if (found == _sessions.end() || found->second->inflight.empty()) {
        connection->closeAfterFlush();
    }
}
