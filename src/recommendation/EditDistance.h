#pragma once

#include <cstddef>
#include <string>

class EditDistance
{
public:
    static std::size_t levenshtein(const std::u32string &left,
                                   const std::u32string &right);
};
