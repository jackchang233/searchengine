#pragma once

#include "RecommendationProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

struct SearchRequest
{
    std::string requestId;
    std::string query;
    std::size_t topK = 0;
};

struct ProtocolSearchHit
{
    std::uint64_t documentId;
    std::string title;
    std::string url;
    std::string snippet;
    double score;
};

struct SearchResponse
{
    bool hasRequestId = false;
    std::string requestId;
    int code = 0;
    std::string message = "ok";
    std::string generation;
    std::size_t candidateCount = 0;
    std::vector<ProtocolSearchHit> data;
};

class SearchProtocol
{
public:
    static const std::uint32_t SearchRequestType = 0x00000002U;
    static const std::uint32_t SearchResponseType = 0x80000002U;

    static SearchRequest decodeRequest(const Frame &frame,
                                       const ProtocolLimits &limits);
    static std::string encodeResponse(const SearchResponse &response,
                                      std::size_t maxResponseBytes);
    static SearchResponse errorResponse(const ProtocolError &error);
};
