#include "RecommendationProtocol.h"

#include "Utf8Utils.h"

#include <nlohmann/json.hpp>

#include <set>
#include <stdexcept>

namespace {

bool validRequestId(const std::string &id)
{
    if (id.empty() || id.size() > 64) {
        return false;
    }
    for (unsigned char character : id) {
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') ||
              character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
}

bool withinJsonDepth(const std::string &body, std::size_t maximum)
{
    std::size_t depth = 0;
    bool inString = false;
    bool escaped = false;
    for (char character : body) {
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{' || character == '[') {
            if (++depth > maximum) {
                return false;
            }
        } else if (character == '}' || character == ']') {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return depth == 0 && !inString && !escaped;
}

void validateLimits(const ProtocolLimits &limits)
{
    if (limits.defaultTopK == 0 || limits.defaultTopK > limits.maxTopK ||
        limits.maxQueryChars == 0 || limits.maxBodyBytes == 0 ||
        limits.maxJsonDepth == 0) {
        throw std::invalid_argument("invalid recommendation protocol limits");
    }
}

} // namespace

ProtocolError::ProtocolError(int code, const std::string &message,
                             const std::string &requestId)
: std::runtime_error(message)
, _code(code)
, _requestId(requestId)
{
}

int ProtocolError::code() const
{
    return _code;
}

bool ProtocolError::hasRequestId() const
{
    return !_requestId.empty();
}

const std::string &ProtocolError::requestId() const
{
    return _requestId;
}

RecommendationRequest RecommendationProtocol::decodeRequest(
    const Frame &frame, const ProtocolLimits &limits,
    std::uint32_t expectedType)
{
    validateLimits(limits);
    if (frame.type == 0 || (frame.type & 0x80000000U) != 0) {
        throw ProtocolError(400, "client frame must be a request");
    }
    if (frame.body.size() > limits.maxBodyBytes ||
        !Utf8Utils::isValid(frame.body) ||
        !withinJsonDepth(frame.body, limits.maxJsonDepth)) {
        throw ProtocolError(400, "invalid JSON encoding, size or depth");
    }

    bool duplicateKey = false;
    std::set<std::string> keys;
    const nlohmann::json::parser_callback_t callback =
        [&](int, nlohmann::json::parse_event_t event,
            nlohmann::json &parsed) -> bool {
        if (event == nlohmann::json::parse_event_t::key &&
            !keys.insert(parsed.get<std::string>()).second) {
            duplicateKey = true;
        }
        return true;
    };
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(frame.body, callback);
    } catch (const nlohmann::json::exception &) {
        throw ProtocolError(400, "invalid JSON body");
    }
    if (duplicateKey || !body.is_object()) {
        throw ProtocolError(400, "duplicate JSON key or non-object request");
    }

    std::string requestId;
    const auto id = body.find("request_id");
    if (id != body.end() && id->is_string() &&
        validRequestId(id->get<std::string>())) {
        requestId = id->get<std::string>();
    }
    const auto version = body.find("version");
    if (version == body.end() || !version->is_number_integer() ||
        *version != 1) {
        throw ProtocolError(400, "version must be integer 1", requestId);
    }
    if (requestId.empty()) {
        throw ProtocolError(400, "invalid or missing request_id");
    }
    if (frame.type != expectedType) {
        throw ProtocolError(501, "unsupported operation", requestId);
    }
    for (auto field = body.begin(); field != body.end(); ++field) {
        if (field.key() != "version" && field.key() != "request_id" &&
            field.key() != "query" && field.key() != "top_k") {
            throw ProtocolError(400, "unknown request field: " + field.key(),
                                requestId);
        }
    }
    const auto query = body.find("query");
    if (query == body.end() || !query->is_string() ||
        query->get<std::string>().empty()) {
        throw ProtocolError(400, "query must be a nonempty string", requestId);
    }
    const std::string queryText = query->get<std::string>();
    if (!Utf8Utils::isValid(queryText) ||
        Utf8Utils::toCodePoints(queryText).size() > limits.maxQueryChars) {
        throw ProtocolError(400, "query encoding or length is invalid", requestId);
    }
    std::size_t topK = limits.defaultTopK;
    const auto top = body.find("top_k");
    if (top != body.end()) {
        if (!top->is_number_integer() || *top < 1 || *top > limits.maxTopK) {
            throw ProtocolError(400, "top_k is outside configured range", requestId);
        }
        topK = top->get<std::size_t>();
    }
    RecommendationRequest request;
    request.requestId = requestId;
    request.query = queryText;
    request.topK = topK;
    return request;
}

std::string RecommendationProtocol::encodeResponse(
    const ProtocolResponse &response, std::size_t maxResponseBytes)
{
    if ((response.type & 0x80000000U) == 0 ||
        (response.type & 0x7fffffffU) == 0 ||
        (response.hasRequestId && !validRequestId(response.requestId))) {
        throw std::invalid_argument("invalid response type or request ID");
    }
    nlohmann::json body;
    body["version"] = 1;
    body["request_id"] = response.hasRequestId
                             ? nlohmann::json(response.requestId)
                             : nlohmann::json(nullptr);
    body["code"] = response.code;
    body["message"] = response.message;
    body["data"] = nlohmann::json::array();
    for (const ProtocolRecommendation &item : response.data) {
        body["data"].push_back({
            {"word", item.word},
            {"distance", item.distance},
            {"frequency", item.frequency}});
    }
    return TlvFrameCodec::encode(Frame{response.type, body.dump()},
                                 maxResponseBytes);
}

ProtocolResponse RecommendationProtocol::errorResponse(
    std::uint32_t requestType, const ProtocolError &error)
{
    if (requestType == 0 || (requestType & 0x80000000U) != 0) {
        throw std::invalid_argument("invalid request type for error response");
    }
    ProtocolResponse response;
    response.type = requestType | 0x80000000U;
    response.hasRequestId = error.hasRequestId();
    response.requestId = error.requestId();
    response.code = error.code();
    response.message = error.what();
    return response;
}
