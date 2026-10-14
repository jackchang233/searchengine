#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

// 离线网页索引和在线查询共享同一套清洗与分词规则。
class TextAnalyzer
{
public:
    explicit TextAnalyzer(const std::string &dictionaryDirectory = std::string());
    ~TextAnalyzer();
    TextAnalyzer(const TextAnalyzer &) = delete;
    TextAnalyzer &operator=(const TextAnalyzer &) = delete;

    std::vector<std::string> tokenize(const std::string &title,
                                      const std::string &content) const;
    static std::set<std::string> loadStopWords(
        const std::vector<std::string> &files);
    static std::string visibleText(const std::string &html);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
