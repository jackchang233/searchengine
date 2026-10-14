#include "Configuration.h"
#include "DictionaryRepository.h"
#include "DictionaryStore.h"
#include "EditDistance.h"
#include "KeywordRecommender.h"
#include "QueryNormalizer.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void expectQueryError(Action action, QueryErrorCode code)
{
    try {
        action();
    } catch (const QueryValidationError &error) {
        require(error.code() == code, "unexpected query error code");
        return;
    }
    throw std::runtime_error("expected query validation error");
}

std::shared_ptr<const DictionaryRepository> loadRepository(const char *config)
{
    const ServerOptions options = Configuration::load(config);
    if (!options.keywordIndexRoot.empty()) {
        return DictionaryStore::loadCurrent(options.keywordIndexRoot).repository;
    }
    return DictionaryRepository::load({
        {"en", options.enDictionaryPath, options.enIndexPath},
        {"cn", options.cnDictionaryPath, options.cnIndexPath}});
}

void testNormalization()
{
    const NormalizedQuery english =
        QueryNormalizer::normalize(" \tHeLLo\r\n", 64);
    require(english.text == "hello" && english.codePoints.size() == 5 &&
                english.uniqueCharacters ==
                    std::vector<std::string>({"h", "e", "l", "o"}),
            "ASCII normalization or repeated character removal failed");
    const NormalizedQuery chinese =
        QueryNormalizer::normalize("中A中", 64);
    require(chinese.text == "中a中" && chinese.codePoints.size() == 3 &&
                chinese.uniqueCharacters.size() == 2,
            "mixed Unicode normalization failed");
    const std::string supplementary("\xF0\xA0\x80\x80");
    require(QueryNormalizer::normalize(supplementary, 1).codePoints.size() == 1,
            "supplementary-plane character counted as multiple code points");

    expectQueryError([&] { QueryNormalizer::normalize(" \t\r", 64); },
                     QueryErrorCode::Empty);
    expectQueryError([&] { QueryNormalizer::normalize("a b", 64); },
                     QueryErrorCode::Whitespace);
    expectQueryError([&] { QueryNormalizer::normalize("a　b", 64); },
                     QueryErrorCode::Whitespace);
    expectQueryError([&] {
        QueryNormalizer::normalize(std::string("a\0b", 3), 64);
    }, QueryErrorCode::Control);
    expectQueryError([&] {
        QueryNormalizer::normalize(std::string("\xFF"), 64);
    }, QueryErrorCode::InvalidUtf8);
    expectQueryError([&] { QueryNormalizer::normalize("abc", 2); },
                     QueryErrorCode::TooLong);
    require(QueryNormalizer::normalize(std::string(64, 'a'), 64)
                .codePoints.size() == 64,
            "maximum-length query was rejected");
    expectQueryError([&] {
        QueryNormalizer::normalize(std::string(65, 'a'), 64);
    }, QueryErrorCode::TooLong);
}

void testDistance()
{
    require(EditDistance::levenshtein(U"", U"abc") == 3 &&
                EditDistance::levenshtein(U"abc", U"") == 3,
            "empty-string edit distance is wrong");
    require(EditDistance::levenshtein(U"kitten", U"sitting") == 3 &&
                EditDistance::levenshtein(U"sitting", U"kitten") == 3,
            "insertion/deletion/replacement distance is wrong");
    require(EditDistance::levenshtein(U"中国", U"中囯") == 1 &&
                EditDistance::levenshtein(U"𠀀", U"中") == 1 &&
                EditDistance::levenshtein(U"相同", U"相同") == 0,
            "Unicode code-point edit distance is wrong");
}

void testOrdering()
{
    const Recommendation near{"near", 1, 1};
    const Recommendation far{"far", 999, 2};
    const Recommendation frequent{"frequent", 8, 1};
    const Recommendation alpha{"a", 8, 1};
    const Recommendation beta{"b", 8, 1};
    require(KeywordRecommender::better(near, far) &&
                KeywordRecommender::better(frequent, near) &&
                KeywordRecommender::better(alpha, beta) &&
                !KeywordRecommender::better(alpha, alpha) &&
                !KeywordRecommender::better(beta, alpha),
            "recommendation comparator is inconsistent");
}

void testFixtureRecommendation()
{
    const auto repository = loadRepository(SEARCHENGINE_RECOMMENDATION_FIXTURE_CONFIG);
    const KeywordRecommender recommender(repository, 64, 20);
    const RecommendationResult tie = recommender.recommend("ad", 3);
    require(tie.candidateCount == 3 && tie.items.size() == 3 &&
                tie.items[0].word == "ab" && tie.items[1].word == "ac" &&
                tie.items[2].word == "aa" && tie.items[0].distance == 1,
            "frequency and lexical tie-breaking failed");
    const RecommendationResult english = recommender.recommend(" HeLo ", 2);
    require(english.normalizedQuery == "helo" &&
                english.items.size() == 2 &&
                english.items[0].word == "hello" &&
                english.items[1].word == "help" &&
                english.items[0].distance == 1,
            "English spelling recommendation failed");
    const RecommendationResult chinese = recommender.recommend("中囯", 3);
    require(chinese.candidateCount == 3 && chinese.items.size() == 3 &&
                chinese.items[0].word == "中华" &&
                chinese.items[1].word == "中国" &&
                chinese.items[2].word == "中文",
            "Chinese typo or frequency ordering failed");
    require(recommender.recommend("中a", 20).candidateCount == 6,
            "mixed-language source IDs were merged");
    const RecommendationResult digit = recommender.recommend("hell0", 2);
    require(!digit.items.empty() && digit.items[0].word == "hello",
            "digit typo was rejected or ranked incorrectly");
    const RecommendationResult empty = recommender.recommend("🧪", 5);
    require(empty.candidateCount == 0 && empty.items.empty(),
            "valid query without shared characters should return empty results");
    expectQueryError([&] { recommender.recommend("hello", 0); },
                     QueryErrorCode::InvalidK);
    expectQueryError([&] { recommender.recommend("hello", 21); },
                     QueryErrorCode::InvalidK);
}

void testRealData()
{
    const auto repository = loadRepository(SEARCHENGINE_TEST_CONFIG);
    const KeywordRecommender recommender(repository, 64, 20);
    const RecommendationResult english = recommender.recommend("hello", 5);
    const RecommendationResult chinese = recommender.recommend("中国", 5);
    require(!english.items.empty() && english.items[0].word == "hello" &&
                english.items[0].distance == 0 &&
                !chinese.items.empty() && chinese.items[0].word == "中国" &&
                chinese.items[0].distance == 0,
            "real-dictionary exact-match smoke test failed");
}

} // namespace

int main()
{
    try {
        testNormalization();
        testDistance();
        testOrdering();
        testFixtureRecommendation();
        testRealData();
    } catch (const std::exception &error) {
        std::cerr << "KeywordRecommenderTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
