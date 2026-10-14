#include "FrameCodec.h"

#include <cstdint>
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

void testFragmentationAndCoalescing()
{
    const Frame first{1, "{\"query\":\"中\n国\"}"};
    const Frame second{2, "{}"};
    const std::string encoded = TlvFrameCodec::encode(first, 4096);
    require(encoded.size() == 8 + first.body.size() &&
                static_cast<unsigned char>(encoded[3]) == 1 &&
                static_cast<unsigned char>(encoded[7]) == first.body.size(),
            "TLV header or UTF-8 byte length is wrong");

    for (std::size_t size = 0; size < encoded.size(); ++size) {
        const FrameDecodeResult partial = TlvFrameCodec::decode(
            encoded.substr(0, size), 4096, FrameDirection::RequestsOnly);
        require(partial.status == FrameDecodeStatus::NeedMore &&
                    partial.consumed == 0,
                "partial TLV frame consumed bytes or completed early");
    }
    const FrameDecodeResult complete = TlvFrameCodec::decode(
        encoded, 4096, FrameDirection::RequestsOnly);
    require(complete.status == FrameDecodeStatus::FrameReady &&
                complete.consumed == encoded.size() &&
                complete.frame.type == 1 && complete.frame.body == first.body,
            "complete TLV frame decoded incorrectly");

    std::string joined = encoded + TlvFrameCodec::encode(second, 4096);
    joined.append(encoded.data(), 5);
    const FrameDecodeResult head = TlvFrameCodec::decode(
        joined, 4096, FrameDirection::RequestsOnly);
    joined.erase(0, head.consumed);
    const FrameDecodeResult next = TlvFrameCodec::decode(
        joined, 4096, FrameDirection::RequestsOnly);
    joined.erase(0, next.consumed);
    require(head.status == FrameDecodeStatus::FrameReady &&
                next.status == FrameDecodeStatus::FrameReady &&
                next.frame.type == 2 &&
                TlvFrameCodec::decode(joined, 4096,
                                      FrameDirection::RequestsOnly).status ==
                    FrameDecodeStatus::NeedMore,
            "coalesced frames or incomplete tail were mishandled");
}

void testLimitsAndDirection()
{
    const std::string zero = TlvFrameCodec::encode(Frame{1, ""}, 4096);
    require(TlvFrameCodec::decode(zero, 4096,
                                  FrameDirection::RequestsOnly).status ==
                FrameDecodeStatus::FrameReady,
            "empty body should remain a valid frame");
    const std::string response =
        TlvFrameCodec::encode(Frame{0x80000001U, "{}"}, 4096);
    require(TlvFrameCodec::decode(response, 4096,
                                  FrameDirection::RequestsOnly).status ==
                FrameDecodeStatus::InvalidFrame &&
                TlvFrameCodec::decode(response, 4096,
                                      FrameDirection::ResponsesOnly).status ==
                    FrameDecodeStatus::FrameReady,
            "request/response direction was not enforced");
    const std::string overlong = TlvFrameCodec::encode(Frame{1, "12345"}, 5);
    require(TlvFrameCodec::decode(overlong.substr(0, 8), 4,
                                  FrameDirection::RequestsOnly).status ==
                FrameDecodeStatus::InvalidFrame,
            "oversized body must be rejected from its header");
    std::string huge = zero;
    huge[4] = static_cast<char>(0xff);
    huge[5] = static_cast<char>(0xff);
    huge[6] = static_cast<char>(0xff);
    huge[7] = static_cast<char>(0xff);
    require(TlvFrameCodec::decode(huge, 4096,
                                  FrameDirection::RequestsOnly).status ==
                FrameDecodeStatus::InvalidFrame,
            "huge untrusted length was accepted");
    try {
        TlvFrameCodec::encode(Frame{0, "{}"}, 4096);
        throw std::runtime_error("zero operation type was encoded");
    } catch (const std::invalid_argument &) {
    }
}

void testLineCompatibility()
{
    const FrameDecodeResult partial = LineFrameCodec::decode("abc", 8);
    const FrameDecodeResult complete = LineFrameCodec::decode("abc\nrest", 8);
    require(partial.status == FrameDecodeStatus::NeedMore &&
                complete.status == FrameDecodeStatus::FrameReady &&
                complete.consumed == 4 && complete.frame.body == "abc\n",
            "line framing changed Echo message bytes");
    require(LineFrameCodec::decode("abcdefghi", 8).status ==
                FrameDecodeStatus::InvalidFrame &&
                LineFrameCodec::decode("abcdefgh\n", 8).status ==
                    FrameDecodeStatus::InvalidFrame,
            "line length limit was not enforced");
}

} // namespace

int main()
{
    try {
        testFragmentationAndCoalescing();
        testLimitsAndDirection();
        testLineCompatibility();
    } catch (const std::exception &error) {
        std::cerr << "FrameCodecTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
