#pragma once

#include "FrameCodec.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct ProtocolLimits
{
    std::size_t defaultTopK = 5;
    std::size_t maxTopK = 20;
    std::size_t maxQueryChars = 64;
    std::size_t maxBodyBytes = 4096;
    std::size_t maxJsonDepth = 8;
};

struct RecommendationRequest
{
    std::string requestId;
    std::string query;
    std::size_t topK = 0;
};

class ProtocolError : public std::runtime_error
{
public:
    ProtocolError(int code, const std::string &message,
                  const std::string &requestId = "");

    int code() const;
    bool hasRequestId() const;
    const std::string &requestId() const;

private:
    int _code;
    std::string _requestId;
};

struct ProtocolRecommendation
{
    std::string word;
    std::size_t distance;
    std::uint64_t frequency;
};

struct ProtocolResponse
{
    std::uint32_t type = 0x80000001U;
    bool hasRequestId = false;
    std::string requestId;
    int code = 0;
    std::string message = "ok";
    std::vector<ProtocolRecommendation> data;
};

class RecommendationProtocol
{
public:
    static const std::uint32_t RecommendRequestType = 0x00000001U;
    static const std::uint32_t RecommendResponseType = 0x80000001U;

    static RecommendationRequest decodeRequest(const Frame &frame,
                                                const ProtocolLimits &limits,
                                                std::uint32_t expectedType =
                                                    RecommendRequestType);
    static std::string encodeResponse(const ProtocolResponse &response,
                                      std::size_t maxResponseBytes);
    static ProtocolResponse errorResponse(std::uint32_t requestType,
                                           const ProtocolError &error);
};
