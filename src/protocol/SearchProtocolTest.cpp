#include "SearchProtocol.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

void expectBad(const std::string &body)
{
    try {
        SearchProtocol::decodeRequest(Frame{2, body}, ProtocolLimits());
    } catch (const ProtocolError &error) {
        require(error.code() == 400, "wrong search request error code");
        return;
    }
    throw std::runtime_error("bad search request accepted");
}

void testProtocol()
{
    const SearchRequest request = SearchProtocol::decodeRequest(
        Frame{2, R"({"version":1,"request_id":"query_1","query":"中国","top_k":3})"},
        ProtocolLimits());
    require(request.requestId == "query_1" && request.query == "中国" &&
                request.topK == 3, "search request decoded incorrectly");
    expectBad(R"({"version":1,"request_id":"bad","query":"a","top_k":0})");
    expectBad(R"({"version":1,"request_id":"bad","query":"a","extra":1})");
    expectBad(R"({"version":1,"request_id":"bad","query":"a","query":"b"})");

    SearchResponse response;
    response.hasRequestId = true;
    response.requestId = "query_1";
    response.generation = "gen-example";
    response.candidateCount = 1;
    response.data.push_back(ProtocolSearchHit{
        1, "标题", "https://example.test", "摘要", 0.5});
    const FrameDecodeResult decoded = TlvFrameCodec::decode(
        SearchProtocol::encodeResponse(response, 4096), 4096,
        FrameDirection::ResponsesOnly);
    require(decoded.status == FrameDecodeStatus::FrameReady &&
                decoded.frame.type == SearchProtocol::SearchResponseType,
            "search response frame is invalid");
    const nlohmann::json body = nlohmann::json::parse(decoded.frame.body);
    require(body["request_id"] == "query_1" && body["data"].size() == 1 &&
                body["data"][0]["docid"] == 1 && body["generation"] ==
                    "gen-example", "search response body is invalid");
}

} // namespace

int main()
{
    try { testProtocol(); }
    catch (const std::exception &error) {
        std::cerr << "SearchProtocolTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
