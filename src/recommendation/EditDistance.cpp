#include "EditDistance.h"

#include <algorithm>
#include <numeric>
#include <vector>

std::size_t EditDistance::levenshtein(const std::u32string &left,
                                      const std::u32string &right)
{
    const std::u32string &longer = left.size() >= right.size() ? left : right;
    const std::u32string &shorter = left.size() >= right.size() ? right : left;
    std::vector<std::size_t> previous(shorter.size() + 1);
    std::vector<std::size_t> current(shorter.size() + 1);
    std::iota(previous.begin(), previous.end(), 0);

    for (std::size_t row = 1; row <= longer.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= shorter.size(); ++column) {
            const std::size_t substitution =
                previous[column - 1] +
                (longer[row - 1] == shorter[column - 1] ? 0 : 1);
            current[column] = std::min(
                std::min(previous[column] + 1, current[column - 1] + 1),
                substitution);
        }
        previous.swap(current);
    }
    return previous.back();
}
