#include "webpage/WebSearcher.h"
#include "Utf8Utils.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace {

std::string truncateUtf8(const std::string &text, std::size_t maxCharacters)
{
    std::string result;
    std::size_t count = 0;
    for (const std::string &character : Utf8Utils::splitCharacters(text)) {
        if (count++ == maxCharacters) break;
        result += character;
    }
    return result;
}

} // namespace

WebSearcher::WebSearcher(
    std::shared_ptr<const WebIndexSnapshot> snapshot,
    const std::string &dictionaryDirectory,
    const std::vector<std::string> &stopWordFiles,
    std::size_t maxQueryChars, std::size_t maxTopK)
: _snapshot(std::move(snapshot))
, _analyzer(dictionaryDirectory, stopWordFiles)
, _maxQueryChars(maxQueryChars)
, _maxTopK(maxTopK)
{
    if (!_snapshot || _maxQueryChars == 0 || _maxTopK == 0) {
        throw std::invalid_argument("web search requires a snapshot and limits");
    }
}

WebSearchResult WebSearcher::search(
    const std::string &query, std::size_t topK) const
{
    if (topK == 0 || topK > _maxTopK || !Utf8Utils::isValid(query) ||
        Utf8Utils::toCodePoints(query).size() > _maxQueryChars) {
        throw std::invalid_argument("invalid web search query or top_k");
    }
    WebSearchResult result;
    result.generation = _snapshot->generation();
    const WebQueryVector analyzed = _analyzer.analyze(query, *_snapshot);
    if (analyzed.hasUnknownTerm || analyzed.weights.empty()) {
        return result;
    }

    const auto &index = _snapshot->invertedIndex();
    auto first = analyzed.weights.begin();
    for (auto term = analyzed.weights.begin(); term != analyzed.weights.end(); ++term) {
        if (index.at(term->first).size() < index.at(first->first).size()) {
            first = term;
        }
    }
    std::map<std::uint64_t, double> scores;
    for (const auto &posting : index.at(first->first)) {
        scores.emplace(posting.first, first->second * posting.second);
    }
    for (const auto &term : analyzed.weights) {
        if (term.first == first->first) continue;
        const auto &postings = index.at(term.first);
        for (auto candidate = scores.begin(); candidate != scores.end();) {
            const auto posting = std::lower_bound(
                postings.begin(), postings.end(), candidate->first,
                [](const PageProcessor::Posting &item, std::uint64_t id) {
                    return item.first < id;
                });
            if (posting == postings.end() || posting->first != candidate->first) {
                candidate = scores.erase(candidate);
            } else {
                candidate->second += term.second * posting->second;
                ++candidate;
            }
        }
        if (scores.empty()) break;
    }
    result.candidateCount = scores.size();
    std::vector<std::pair<std::uint64_t, double>> ranked(scores.begin(),
                                                          scores.end());
    std::sort(ranked.begin(), ranked.end(), [](
        const std::pair<std::uint64_t, double> &left,
        const std::pair<std::uint64_t, double> &right) {
        return left.second != right.second ? left.second > right.second :
                                             left.first < right.first;
    });
    if (ranked.size() > topK) ranked.resize(topK);
    for (const auto &entry : ranked) {
        const PageProcessor::Document document =
            _snapshot->readDocument(entry.first);
        result.hits.push_back(WebSearchHit{
            document.id, truncateUtf8(document.title, 256),
            truncateUtf8(document.link, 1024),
            WebQueryAnalyzer::snippet(document.content, 180), entry.second});
    }
    return result;
}
