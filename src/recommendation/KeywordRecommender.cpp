#include "KeywordRecommender.h"

#include "EditDistance.h"
#include "QueryNormalizer.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

KeywordRecommender::KeywordRecommender(
    std::shared_ptr<const DictionaryRepository> repository,
    std::size_t maxQueryChars, std::size_t maxTopK)
: _repository(std::move(repository))
, _maxQueryChars(maxQueryChars)
, _maxTopK(maxTopK)
{
    if (!_repository || _maxQueryChars == 0 || _maxTopK == 0) {
        throw std::invalid_argument("recommender needs a repository and positive limits");
    }
}

bool KeywordRecommender::better(const Recommendation &left,
                                const Recommendation &right)
{
    if (left.distance != right.distance) {
        return left.distance < right.distance;
    }
    if (left.frequency != right.frequency) {
        return left.frequency > right.frequency;
    }
    return left.word < right.word;
}

RecommendationResult KeywordRecommender::recommend(
    const std::string &rawQuery, std::size_t topK) const
{
    if (topK == 0 || topK > _maxTopK) {
        throw QueryValidationError(QueryErrorCode::InvalidK,
                                   "top K must be between 1 and the configured maximum");
    }
    const NormalizedQuery query =
        QueryNormalizer::normalize(rawQuery, _maxQueryChars);
    const std::vector<WordId> candidates =
        _repository->findCandidates(query.uniqueCharacters);
    RecommendationResult result;
    result.normalizedQuery = query.text;
    result.candidateCount = candidates.size();
    result.items.reserve(candidates.size());
    for (const WordId &id : candidates) {
        const WordEntry &entry = _repository->word(id);
        result.items.push_back(Recommendation{
            entry.word, entry.frequency,
            EditDistance::levenshtein(query.codePoints, entry.codePoints)});
    }
    std::sort(result.items.begin(), result.items.end(), better);
    if (result.items.size() > topK) {
        result.items.resize(topK);
    }
    return result;
}
