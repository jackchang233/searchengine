#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

enum class QueryErrorCode
{
    InvalidUtf8,
    Empty,
    Whitespace,
    Control,
    TooLong,
    InvalidK
};

class QueryValidationError : public std::invalid_argument
{
public:
    QueryValidationError(QueryErrorCode code, const std::string &message);
    QueryErrorCode code() const;

private:
    QueryErrorCode _code;
};

const char *queryErrorName(QueryErrorCode code);

struct NormalizedQuery
{
    std::string text;
    std::u32string codePoints;
    std::vector<std::string> uniqueCharacters;
};

class QueryNormalizer
{
public:
    static NormalizedQuery normalize(const std::string &raw,
                                     std::size_t maxCodePoints);
};
