#pragma once

#include "webpage/TextAnalyzer.h"
#include "webpage/WebIndexStore.h"

#include <map>
#include <set>
#include <string>
#include <vector>

struct WebQueryVector
{
    std::map<std::string, double> weights;
    bool hasUnknownTerm = false;
};

class WebQueryAnalyzer
{
public:
    WebQueryAnalyzer(const std::string &dictionaryDirectory,
                     const std::vector<std::string> &stopWordFiles);
    WebQueryVector analyze(const std::string &query,
                           const WebIndexSnapshot &snapshot) const;
    static std::string snippet(const std::string &content,
                               std::size_t maxCharacters);

private:
    TextAnalyzer _analyzer;
    std::set<std::string> _stopWords;
};
