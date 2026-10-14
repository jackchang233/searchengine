#include "Configuration.h"
#include "DictionaryRepository.h"
#include "DictionaryStore.h"
#include "KeywordRecommender.h"
#include "QueryNormalizer.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Arguments
{
    std::string config = "conf/myconf.conf";
    std::string query;
    std::string topK;
    bool hasQuery = false;
    bool hasTopK = false;
};

void printUsage(const char *program)
{
    std::cerr << "usage: " << program
              << " [--config FILE] --query WORD [--top-k NUMBER]\n";
}

Arguments parseArguments(int argc, char *argv[])
{
    Arguments result;
    bool hasConfig = false;
    for (int index = 1; index < argc; index += 2) {
        const std::string option = argv[index];
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + option);
        }
        const std::string value = argv[index + 1];
        if (option == "--config" && !hasConfig) {
            result.config = value;
            hasConfig = true;
        } else if (option == "--query" && !result.hasQuery) {
            result.query = value;
            result.hasQuery = true;
        } else if (option == "--top-k" && !result.hasTopK) {
            result.topK = value;
            result.hasTopK = true;
        } else {
            throw std::invalid_argument("unknown or duplicate option: " + option);
        }
    }
    if (!result.hasQuery || result.config.empty()) {
        throw std::invalid_argument("--query and a nonempty config path are required");
    }
    return result;
}

std::size_t parseTopK(const std::string &value)
{
    if (value.empty()) {
        throw QueryValidationError(QueryErrorCode::InvalidK,
                                   "top K must be a positive decimal integer");
    }
    std::size_t result = 0;
    for (char character : value) {
        if (character < '0' || character > '9') {
            throw QueryValidationError(QueryErrorCode::InvalidK,
                                       "top K must be a positive decimal integer");
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            throw QueryValidationError(QueryErrorCode::InvalidK,
                                       "top K is too large");
        }
        result = result * 10 + digit;
    }
    if (result == 0) {
        throw QueryValidationError(QueryErrorCode::InvalidK,
                                   "top K must be positive");
    }
    return result;
}

int run(const Arguments &arguments)
{
    const ServerOptions options = Configuration::load(arguments.config);
    const std::size_t topK = arguments.hasTopK
                                 ? parseTopK(arguments.topK)
                                 : options.defaultTopK;
    const std::shared_ptr<const DictionaryRepository> repository =
        options.keywordIndexRoot.empty()
            ? DictionaryRepository::load({
                {"en", options.enDictionaryPath, options.enIndexPath},
                {"cn", options.cnDictionaryPath, options.cnIndexPath}})
            : DictionaryStore::loadCurrent(options.keywordIndexRoot).repository;
    const KeywordRecommender recommender(repository, options.maxQueryChars,
                                         options.maxTopK);
    const RecommendationResult result =
        recommender.recommend(arguments.query, topK);

    nlohmann::json output;
    output["query"] = arguments.query;
    output["normalized_query"] = result.normalizedQuery;
    output["candidate_count"] = result.candidateCount;
    output["results"] = nlohmann::json::array();
    for (const Recommendation &item : result.items) {
        output["results"].push_back({
            {"word", item.word},
            {"frequency", item.frequency},
            {"distance", item.distance}});
    }
    std::cout << output.dump() << '\n';
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        return run(parseArguments(argc, argv));
    } catch (const QueryValidationError &error) {
        std::cerr << queryErrorName(error.code()) << ": " << error.what() << '\n';
        return 2;
    } catch (const std::invalid_argument &error) {
        std::cerr << "invalid_arguments: " << error.what() << '\n';
        printUsage(argv[0]);
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "recommendation_failed: " << error.what() << '\n';
        return 1;
    }
}
