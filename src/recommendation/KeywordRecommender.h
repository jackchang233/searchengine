#pragma once

#include "DictionaryRepository.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Recommendation
{
    std::string word;
    std::uint64_t frequency;
    std::size_t distance;
};

struct RecommendationResult
{
    std::string normalizedQuery;
    std::size_t candidateCount;
    std::vector<Recommendation> items;
};

class KeywordRecommender
{
public:
    KeywordRecommender(std::shared_ptr<const DictionaryRepository> repository,
                       std::size_t maxQueryChars, std::size_t maxTopK);

    RecommendationResult recommend(const std::string &rawQuery,
                                    std::size_t topK) const;
    static bool better(const Recommendation &left,
                       const Recommendation &right);

private:
    std::shared_ptr<const DictionaryRepository> _repository;
    std::size_t _maxQueryChars;
    std::size_t _maxTopK;
};
