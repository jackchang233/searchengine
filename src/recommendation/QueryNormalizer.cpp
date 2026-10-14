#include "QueryNormalizer.h"

#include "Utf8Utils.h"

#include <set>

namespace {

bool isAsciiWhitespace(unsigned char character)
{
    return character == 0x20 ||
           (character >= 0x09 && character <= 0x0D);
}

bool isUnicodeWhitespace(char32_t point)
{
    return (point >= 0x09 && point <= 0x0D) || point == 0x20 ||
           point == 0x85 || point == 0xA0 || point == 0x1680 ||
           (point >= 0x2000 && point <= 0x200A) || point == 0x2028 ||
           point == 0x2029 || point == 0x202F || point == 0x205F ||
           point == 0x3000;
}

bool isControl(char32_t point)
{
    return point <= 0x1F || (point >= 0x7F && point <= 0x9F);
}

} // namespace

QueryValidationError::QueryValidationError(QueryErrorCode code,
                                           const std::string &message)
: std::invalid_argument(message)
, _code(code)
{
}

QueryErrorCode QueryValidationError::code() const
{
    return _code;
}

const char *queryErrorName(QueryErrorCode code)
{
    switch (code) {
    case QueryErrorCode::InvalidUtf8: return "invalid_utf8";
    case QueryErrorCode::Empty: return "empty_query";
    case QueryErrorCode::Whitespace: return "query_whitespace";
    case QueryErrorCode::Control: return "query_control";
    case QueryErrorCode::TooLong: return "query_too_long";
    case QueryErrorCode::InvalidK: return "invalid_top_k";
    }
    return "unknown_query_error";
}

NormalizedQuery QueryNormalizer::normalize(const std::string &raw,
                                           std::size_t maxCodePoints)
{
    if (maxCodePoints == 0) {
        throw std::invalid_argument("maxCodePoints must be positive");
    }
    if (!Utf8Utils::isValid(raw)) {
        throw QueryValidationError(QueryErrorCode::InvalidUtf8,
                                   "query is not valid UTF-8");
    }

    std::size_t first = 0;
    std::size_t last = raw.size();
    while (first < last &&
           isAsciiWhitespace(static_cast<unsigned char>(raw[first]))) {
        ++first;
    }
    while (last > first &&
           isAsciiWhitespace(static_cast<unsigned char>(raw[last - 1]))) {
        --last;
    }
    if (first == last) {
        throw QueryValidationError(QueryErrorCode::Empty,
                                   "query is empty after trimming");
    }

    NormalizedQuery result;
    result.text = raw.substr(first, last - first);
    for (char &character : result.text) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    result.codePoints = Utf8Utils::toCodePoints(result.text);
    for (char32_t point : result.codePoints) {
        if (isUnicodeWhitespace(point)) {
            throw QueryValidationError(QueryErrorCode::Whitespace,
                                       "query contains whitespace");
        }
        if (isControl(point)) {
            throw QueryValidationError(QueryErrorCode::Control,
                                       "query contains a control character");
        }
    }
    if (result.codePoints.size() > maxCodePoints) {
        throw QueryValidationError(QueryErrorCode::TooLong,
                                   "query exceeds maximum code point length");
    }
    std::set<std::string> seen;
    for (const std::string &character : Utf8Utils::splitCharacters(result.text)) {
        if (seen.insert(character).second) {
            result.uniqueCharacters.push_back(character);
        }
    }
    return result;
}
