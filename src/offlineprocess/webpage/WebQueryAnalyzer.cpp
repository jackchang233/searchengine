#include "webpage/WebQueryAnalyzer.h"
#include "Utf8Utils.h"

#include <cmath>
#include <stdexcept>

WebQueryAnalyzer::WebQueryAnalyzer(
    const std::string &dictionaryDirectory,
    const std::vector<std::string> &stopWordFiles)
: _analyzer(dictionaryDirectory)
, _stopWords(TextAnalyzer::loadStopWords(stopWordFiles))
{
}

WebQueryVector WebQueryAnalyzer::analyze(
    const std::string &query, const WebIndexSnapshot &snapshot) const
{
    if (!Utf8Utils::isValid(query)) {
        throw std::invalid_argument("query is not valid UTF-8");
    }
    std::map<std::string, std::size_t> frequencies;
    for (const std::string &word : _analyzer.tokenize("", query)) {
        if (_stopWords.count(word) == 0) {
            ++frequencies[word];
        }
    }
    if (frequencies.empty()) {
        throw std::invalid_argument("query has no searchable terms");
    }

    WebQueryVector result;
    const auto &index = snapshot.invertedIndex();
    const double documentCount = static_cast<double>(snapshot.offsets().size());
    double squaredLength = 0.0;
    for (const auto &entry : frequencies) {
        const auto postings = index.find(entry.first);
        if (postings == index.end()) {
            result.hasUnknownTerm = true;
            continue;
        }
        const double idf = std::log2(
            documentCount / static_cast<double>(postings->second.size()));
        const double weight = static_cast<double>(entry.second) * idf;
        if (weight > 0.0) {
            result.weights.emplace(entry.first, weight);
            squaredLength += weight * weight;
        }
    }
    if (squaredLength > 0.0) {
        const double length = std::sqrt(squaredLength);
        for (auto &entry : result.weights) {
            entry.second /= length;
        }
    }
    return result;
}

std::string WebQueryAnalyzer::snippet(
    const std::string &content, std::size_t maxCharacters)
{
    const std::string visible = TextAnalyzer::visibleText(content);
    std::string compact;
    bool previousSpace = true;
    for (const std::string &character : Utf8Utils::splitCharacters(visible)) {
        const bool space = character == " " || character == "\n" ||
                           character == "\r" || character == "\t";
        if (space) {
            if (!previousSpace) compact.push_back(' ');
        } else {
            compact += character;
        }
        previousSpace = space;
    }
    if (!compact.empty() && compact.back() == ' ') compact.pop_back();
    std::string result;
    std::size_t count = 0;
    for (const std::string &character : Utf8Utils::splitCharacters(compact)) {
        if (count++ == maxCharacters) break;
        result += character;
    }
    return result;
}
