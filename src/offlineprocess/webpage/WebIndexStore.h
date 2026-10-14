#pragma once

#include "webpage/PageProcessor.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct WebIndexBuildInputs
{
    std::vector<std::string> xmlFiles;
    std::vector<std::string> stopWordFiles;
    std::string jiebaDictionaryDirectory;
    std::size_t simhashTopN = 20;
    unsigned short hammingThreshold = 3;
};

/** 一次加载一个不可变 generation；在线查询可以共享该只读快照。 */
class WebIndexSnapshot
{
public:
    struct Offset
    {
        std::uint64_t begin;
        std::uint64_t length;
    };

    const std::string &generation() const { return _generation; }
    const std::vector<Offset> &offsets() const { return _offsets; }
    const PageProcessor::InvertedIndex &invertedIndex() const { return _index; }
    PageProcessor::Document readDocument(std::uint64_t documentId) const;
    void validateAnalyzerInputs(
        const std::string &dictionaryDirectory,
        const std::vector<std::string> &stopWordFiles) const;

private:
    friend class WebIndexStore;
    std::string _generation;
    std::string _pageFile;
    std::shared_ptr<int> _pageDescriptor;
    std::vector<Offset> _offsets;
    PageProcessor::InvertedIndex _index;
    struct InputFingerprint
    {
        std::uint64_t size;
        std::string sha256;
    };
    std::vector<InputFingerprint> _stopWordFingerprints;
    std::vector<InputFingerprint> _jiebaFingerprints;
};

/** 完整校验后发布三份网页产物；CURRENT 是唯一的版本切换点。 */
class WebIndexStore
{
public:
    static WebIndexSnapshot publish(const PageProcessor &processor,
                                    const std::string &root,
                                    const WebIndexBuildInputs &inputs);
    static WebIndexSnapshot loadCurrent(const std::string &root);

private:
    static WebIndexSnapshot loadGeneration(const std::string &directory,
                                           const std::string &generation);
};
