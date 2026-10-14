#include "SearchProtocol.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <stdexcept>

SearchRequest SearchProtocol::decodeRequest(
    const Frame &frame, const ProtocolLimits &limits)
{
    const RecommendationRequest common =
        RecommendationProtocol::decodeRequest(frame, limits, SearchRequestType);
    SearchRequest request;
    request.requestId = common.requestId;
    request.query = common.query;
    request.topK = common.topK;
    return request;
}

std::string SearchProtocol::encodeResponse(
    const SearchResponse &response, std::size_t maxResponseBytes)
{
    if (response.hasRequestId && response.requestId.empty()) {
        throw std::invalid_argument("search response needs request_id");
    }
    nlohmann::json body;
    body["version"] = 1;
    body["request_id"] = response.hasRequestId
        ? nlohmann::json(response.requestId) : nlohmann::json(nullptr);
    body["code"] = response.code;
    body["message"] = response.message;
    body["generation"] = response.generation;
    body["candidate_count"] = response.candidateCount;
    body["data"] = nlohmann::json::array();
    for (const ProtocolSearchHit &item : response.data) {
        if (!std::isfinite(item.score) || item.score < 0.0) {
            throw std::invalid_argument("invalid search score");
        }
        body["data"].push_back({
            {"docid", item.documentId}, {"title", item.title},
            {"url", item.url}, {"snippet", item.snippet},
            {"score", item.score}});
    }
    return TlvFrameCodec::encode(
        Frame{SearchResponseType, body.dump()}, maxResponseBytes);
}

SearchResponse SearchProtocol::errorResponse(const ProtocolError &error)
{
    SearchResponse response;
    response.hasRequestId = error.hasRequestId();
    response.requestId = error.requestId();
    response.code = error.code();
    response.message = error.what();
    return response;
}
