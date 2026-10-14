#include "RecommendationProtocol.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void expectError(const Frame &frame, int code, bool hasRequestId)
{
    try {
        RecommendationProtocol::decodeRequest(frame, ProtocolLimits());
    } catch (const ProtocolError &error) {
        require(error.code() == code && error.hasRequestId() == hasRequestId,
                "wrong protocol error code or request ID state");
        return;
    }
    throw std::runtime_error("expected protocol validation error");
}

void testRequests()
{
    const Frame request{1,
        "{\"version\":1,\"request_id\":\"req_1\","
        "\"query\":\"中囯\",\"top_k\":5}"};
    const RecommendationRequest decoded =
        RecommendationProtocol::decodeRequest(request, ProtocolLimits());
    require(decoded.requestId == "req_1" && decoded.query == "中囯" &&
                decoded.topK == 5,
            "valid Chinese request decoded incorrectly");
    const Frame defaultK{1,
        "{\"version\":1,\"request_id\":\"a-2\","
        "\"query\":\"a\\n\\\"b\"}"};
    require(RecommendationProtocol::decodeRequest(defaultK, ProtocolLimits())
                .topK == 5,
            "default K or escaped JSON content was mishandled");
    const Frame bracesInString{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"{[\\\"x\\\"]}\"}"};
    require(RecommendationProtocol::decodeRequest(bracesInString,
                                                   ProtocolLimits()).query ==
                "{[\"x\"]}",
            "JSON depth scan counted braces inside a string");
}

void testBadRequests()
{
    expectError(Frame{1, ""}, 400, false);
    expectError(Frame{1, "[1,2]"}, 400, false);
    expectError(Frame{1, "{\"version\":1,\"request_id\":\"x\","},
                400, false);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a\",\"top_k\":-1}"}, 400, true);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a\",\"top_k\":1.5}"}, 400, true);
    expectError(Frame{1,
        "{\"version\":2,\"request_id\":\"x\",\"query\":\"a\"}"},
        400, true);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x!\",\"query\":\"a\"}"},
        400, false);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a\",\"extra\":1}"}, 400, true);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a\",\"query\":\"b\"}"}, 400, false);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":[\"a\"]}"}, 400, true);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a\",\"extra\":[[[[[[[[0]]]]]]]]}"},
        400, false);
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"a" + std::string("\xFF") + "\"}"}, 400, false);
    expectError(Frame{2,
        "{\"version\":1,\"request_id\":\"x\",\"query\":\"a\"}"},
        501, true);
    expectError(Frame{2,
        "{\"version\":1,\"request_id\":\"x\",\"other_operation_data\":42}"},
        501, true);
    const std::string longQuery(65, 'a');
    expectError(Frame{1,
        "{\"version\":1,\"request_id\":\"x\","
        "\"query\":\"" + longQuery + "\"}"}, 400, true);
}

void testResponse()
{
    ProtocolResponse success;
    success.hasRequestId = true;
    success.requestId = "req_1";
    success.data.push_back(ProtocolRecommendation{"中国", 1, 30});
    const std::string bytes =
        RecommendationProtocol::encodeResponse(success, 4096);
    const FrameDecodeResult decoded = TlvFrameCodec::decode(
        bytes, 4096, FrameDirection::ResponsesOnly);
    require(decoded.status == FrameDecodeStatus::FrameReady &&
                decoded.frame.type == 0x80000001U,
            "success response frame is invalid");
    const nlohmann::json body = nlohmann::json::parse(decoded.frame.body);
    require(body["request_id"] == "req_1" && body["code"] == 0 &&
                body["data"].size() == 1 &&
                body["data"][0]["word"] == "中国",
            "success JSON response is invalid");

    const ProtocolError error(400, "bad JSON");
    const ProtocolResponse failure =
        RecommendationProtocol::errorResponse(1, error);
    const FrameDecodeResult errorFrame = TlvFrameCodec::decode(
        RecommendationProtocol::encodeResponse(failure, 4096),
        4096, FrameDirection::ResponsesOnly);
    const nlohmann::json errorBody =
        nlohmann::json::parse(errorFrame.frame.body);
    require(errorBody["request_id"].is_null() && errorBody["code"] == 400 &&
                errorBody["data"].empty(),
            "error response lost null request ID or empty result");
    try {
        RecommendationProtocol::encodeResponse(success, 8);
        throw std::runtime_error("oversized response was encoded");
    } catch (const std::invalid_argument &) {
    }
}

} // namespace

int main()
{
    try {
        testRequests();
        testBadRequests();
        testResponse();
    } catch (const std::exception &error) {
        std::cerr << "RecommendationProtocolTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
