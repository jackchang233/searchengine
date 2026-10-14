#pragma once

// 语料处理模块：英文关键词词典与字符索引。

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class KeywordProcessor
{
public:
    using WordFrequency = std::pair<std::string, std::uint64_t>;
    using Dictionary = std::vector<WordFrequency>;
    using Index = std::map<char, std::vector<std::size_t>>;

    /**
     * 读取英文语料并在内存中建立词典和字符索引。
     *
     * 英文单词由连续的 ASCII 字母组成，统一转换为小写；数字、标点和
     * 其他字符都视为分隔符。每次调用都会清空上一次的处理结果。
     */
    void processEnglish(const std::vector<std::string> &corpusFiles,
                        const std::string &stopWordsFile);

    /** 将词典和索引写入磁盘；词典下标从 0 开始。 */
    void storeEnglish(const std::string &dictionaryFile,
                      const std::string &indexFile) const;

    const Dictionary &englishDictionary() const;
    const Index &englishIndex() const;
    const std::set<std::string> &englishStopWords() const;

private:
    void loadEnglishStopWords(const std::string &filename);
    void countEnglishWords(
        const std::string &filename,
        std::map<std::string, std::uint64_t> &frequencies) const;
    void buildEnglishIndex();

private:
    std::set<std::string> _englishStopWords;
    Dictionary _englishDictionary;
    Index _englishIndex;
};
