#pragma once

#include "corpus/ChineseTokenizer.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class ChineseKeywordProcessor
{
public:
    using WordFrequency = std::pair<std::string, std::uint64_t>;
    using Dictionary = std::vector<WordFrequency>;
    using Index = std::map<std::string, std::vector<std::size_t>>;

    explicit ChineseKeywordProcessor(
        const std::string &jiebaDictionaryDirectory = std::string());

    /**
     * 按行读取 UTF-8 全文，标点和数字作为边界，ASCII 字母转为小写。
     * Mix 分词后过滤停用词和不含汉字的词，保留有效单字及中英混合词。
     * 每次成功调用替换旧结果；读取失败或编码错误时保留上次成功结果。
     */
    void process(const std::vector<std::string> &corpusFiles,
                 const std::string &stopWordsFile);

    /** 写入“词语 词频”和“字符 词典编号”；编号为词典的零基下标。 */
    void store(const std::string &dictionaryFile,
               const std::string &indexFile) const;

    const Dictionary &dictionary() const;
    const Index &index() const;
    const std::set<std::string> &stopWords() const;

private:
    static std::set<std::string> loadStopWords(const std::string &filename);
    void countFile(const std::string &filename,
                   const std::set<std::string> &stopWords,
                   std::map<std::string, std::uint64_t> &frequencies) const;
    static bool shouldKeep(const std::string &word,
                           const std::set<std::string> &stopWords);
    static Index buildIndex(const Dictionary &dictionary);

private:
    ChineseTokenizer _tokenizer;
    std::set<std::string> _stopWords;
    Dictionary _dictionary;
    Index _index;
    bool _ready = false;
};
