#pragma once

// 网页库处理模块：XML 提取、去重、网页持久化和 TF-IDF 倒排索引。

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/** 从 XML 语料提取、去重并生成网页库、网页偏移库和倒排索引。 */
class PageProcessor
{
public:
    using Posting = std::pair<std::uint64_t, double>;
    using InvertedIndex = std::map<std::string, std::vector<Posting>>;

    struct Document
    {
        std::uint64_t id;
        std::string link;
        std::string title;
        std::string content;
    };

    struct Statistics
    {
        std::size_t extracted;
        std::size_t ignored;
        std::size_t duplicates;
    };

    /**
     * topN 是 SimHash 提取的特征数；hammingThreshold 为重复判定阈值。
     * 空资源目录使用 CMake 检测到的 cppjieba 资源目录。
     */
    explicit PageProcessor(
        const std::string &jiebaDictionaryDirectory = std::string(),
        std::size_t topN = 20,
        unsigned short hammingThreshold = 3);
    ~PageProcessor();

    PageProcessor(const PageProcessor &) = delete;
    PageProcessor &operator=(const PageProcessor &) = delete;

    /**
     * 按文件列表和 item 出现顺序处理。content 标签优先；缺少 content 时
     * 使用 description；两者都缺少的 item 被忽略。相似文档保留第一篇。
     */
    void process(const std::vector<std::string> &xmlFiles);

    /**
     * 网页库按 <doc> 片段连续写入；偏移库每行为“文档编号 字节偏移 字节数”。
     */
    void store(const std::string &pageLibraryFile,
               const std::string &offsetLibraryFile) const;

    /**
     * 对标题和清洗后的正文分词，过滤停用词，并按
     * TF * log2(N / DF) 计算、按文档 L2 归一化权重。
     */
    void buildInvertedIndex(const std::vector<std::string> &stopWordFiles);

    /**
     * 每行格式为“词项 文档编号 归一化权重 [文档编号 权重]...”。
     */
    void storeInvertedIndex(const std::string &invertedIndexFile) const;

    const std::vector<Document> &documents() const;
    const Statistics &statistics() const;
    const InvertedIndex &invertedIndex() const;

    static std::string defaultDictionaryDirectory();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
    std::vector<Document> _documents;
    InvertedIndex _invertedIndex;
    Statistics _statistics{0, 0, 0};
    bool _ready = false;
    bool _indexReady = false;
};
