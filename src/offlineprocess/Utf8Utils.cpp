#include "Utf8Utils.h"

#include <utf8cpp/utf8.h>

#include <iterator>
#include <stdexcept>

bool Utf8Utils::isValid(const std::string &text)
{
    return utf8::is_valid(text.begin(), text.end());
}

std::vector<std::string> Utf8Utils::splitCharacters(const std::string &text)
{
    if (!isValid(text)) {
        throw std::invalid_argument("invalid UTF-8 text");
    }

    std::vector<std::string> characters;
    auto current = text.begin();
    while (current != text.end()) {
        const auto begin = current;
        utf8::next(current, text.end());
        characters.emplace_back(begin, current);
    }
    return characters;
}

std::u32string Utf8Utils::toCodePoints(const std::string &text)
{
    if (!isValid(text)) {
        throw std::invalid_argument("invalid UTF-8 text");
    }

    std::u32string result;
    utf8::utf8to32(text.begin(), text.end(), std::back_inserter(result));
    return result;
}

bool Utf8Utils::isHan(char32_t codePoint)
{
    return (codePoint >= 0x3400 && codePoint <= 0x4DBF) ||
           (codePoint >= 0x4E00 && codePoint <= 0x9FFF) ||
           (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
           (codePoint >= 0x20000 && codePoint <= 0x2FA1F) ||
           (codePoint >= 0x30000 && codePoint <= 0x323AF);
}
