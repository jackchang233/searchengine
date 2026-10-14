#include "FrameCodec.h"

#include <limits>
#include <stdexcept>

namespace {

std::uint32_t readUint32(const char *bytes)
{
    return (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes[0])) << 24) |
           (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes[1])) << 16) |
           (static_cast<std::uint32_t>(
                static_cast<unsigned char>(bytes[2])) << 8) |
           static_cast<std::uint32_t>(
               static_cast<unsigned char>(bytes[3]));
}

void appendUint32(std::string &output, std::uint32_t value)
{
    output.push_back(static_cast<char>((value >> 24) & 0xff));
    output.push_back(static_cast<char>((value >> 16) & 0xff));
    output.push_back(static_cast<char>((value >> 8) & 0xff));
    output.push_back(static_cast<char>(value & 0xff));
}

bool validType(std::uint32_t type, FrameDirection direction)
{
    if ((type & 0x7fffffffU) == 0) {
        return false;
    }
    const bool response = (type & 0x80000000U) != 0;
    return direction == FrameDirection::Any ||
           (direction == FrameDirection::RequestsOnly && !response) ||
           (direction == FrameDirection::ResponsesOnly && response);
}

} // namespace

FrameDecodeResult TlvFrameCodec::decode(const std::string &buffer,
                                        std::size_t maxBodyBytes,
                                        FrameDirection direction)
{
    FrameDecodeResult result;
    if (buffer.size() < HeaderSize) {
        return result;
    }
    const std::uint32_t type = readUint32(buffer.data());
    const std::uint32_t bodyLength = readUint32(buffer.data() + 4);
    if (!validType(type, direction) || bodyLength > maxBodyBytes ||
        bodyLength > std::numeric_limits<std::size_t>::max() - HeaderSize) {
        result.status = FrameDecodeStatus::InvalidFrame;
        return result;
    }
    const std::size_t total = HeaderSize + static_cast<std::size_t>(bodyLength);
    if (buffer.size() < total) {
        return result;
    }
    result.status = FrameDecodeStatus::FrameReady;
    result.consumed = total;
    result.frame.type = type;
    result.frame.body.assign(buffer.data() + HeaderSize, bodyLength);
    return result;
}

std::string TlvFrameCodec::encode(const Frame &frame,
                                  std::size_t maxBodyBytes)
{
    if (!validType(frame.type, FrameDirection::Any) ||
        frame.body.size() > maxBodyBytes ||
        frame.body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("invalid TLV type or body length");
    }
    std::string output;
    output.reserve(HeaderSize + frame.body.size());
    appendUint32(output, frame.type);
    appendUint32(output, static_cast<std::uint32_t>(frame.body.size()));
    output.append(frame.body);
    return output;
}

FrameDecodeResult LineFrameCodec::decode(const std::string &buffer,
                                         std::size_t maxLineBytes)
{
    FrameDecodeResult result;
    const std::size_t newline = buffer.find('\n');
    if (newline == std::string::npos) {
        if (buffer.size() > maxLineBytes) {
            result.status = FrameDecodeStatus::InvalidFrame;
        }
        return result;
    }
    if (newline >= maxLineBytes) {
        result.status = FrameDecodeStatus::InvalidFrame;
        return result;
    }
    result.status = FrameDecodeStatus::FrameReady;
    result.consumed = newline + 1;
    result.frame.body = buffer.substr(0, result.consumed);
    return result;
}
