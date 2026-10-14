#pragma once

#include <string>
#include <vector>

/** 按 Unicode 码点处理 UTF-8，供离线索引和在线查询复用。 */
class Utf8Utils
{
public:
    static bool isValid(const std::string &text);

    /** 返回完整字符的 UTF-8 字节串；非法编码抛出 std::invalid_argument。 */
    static std::vector<std::string> splitCharacters(const std::string &text);
    static std::u32string toCodePoints(const std::string &text);

    /** 中文建库接受的 CJK 汉字区段，包括常用字、兼容字和扩展区。 */
    static bool isHan(char32_t codePoint);

private:
    Utf8Utils() = delete;
};
