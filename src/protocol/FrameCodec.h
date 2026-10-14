#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

struct Frame
{
    Frame() : type(0), body() {}
    Frame(std::uint32_t frameType, std::string frameBody)
    : type(frameType), body(std::move(frameBody)) {}

    std::uint32_t type;
    std::string body;
};

enum class FrameDecodeStatus
{
    NeedMore,
    FrameReady,
    InvalidFrame
};

enum class FrameDirection
{
    Any,
    RequestsOnly,
    ResponsesOnly
};

enum class FrameMode
{
    Line,
    Tlv
};

struct FrameDecodeResult
{
    FrameDecodeStatus status = FrameDecodeStatus::NeedMore;
    std::size_t consumed = 0;
    Frame frame;
};

class TlvFrameCodec
{
public:
    static const std::size_t HeaderSize = 8;

    static FrameDecodeResult decode(const std::string &buffer,
                                    std::size_t maxBodyBytes,
                                    FrameDirection direction);
    static std::string encode(const Frame &frame,
                              std::size_t maxBodyBytes);
};

class LineFrameCodec
{
public:
    // 保留换行符，确保 Echo 的既有响应字节不变。
    static FrameDecodeResult decode(const std::string &buffer,
                                    std::size_t maxLineBytes);
};
