#pragma once

#include "webpage/WebQueryAnalyzer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct WebSearchHit
{
    std::uint64_t documentId;
    std::string title;
    std::string url;
    std::string snippet;
    double score;
};

struct WebSearchResult
{
    std::string generation;
    std::size_t candidateCount = 0;
    std::vector<WebSearchHit> hits;
};

class WebSearcher
{
public:
    WebSearcher(std::shared_ptr<const WebIndexSnapshot> snapshot,
                const std::string &dictionaryDirectory,
                const std::vector<std::string> &stopWordFiles,
                std::size_t maxQueryChars,
                std::size_t maxTopK);
    WebSearchResult search(const std::string &query, std::size_t topK) const;

private:
    std::shared_ptr<const WebIndexSnapshot> _snapshot;
    WebQueryAnalyzer _analyzer;
    std::size_t _maxQueryChars;
    std::size_t _maxTopK;
};
