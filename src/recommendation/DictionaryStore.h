#pragma once

#include "DictionaryRepository.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct DictionaryFiles
{
    std::string englishDictionary;
    std::string englishIndex;
    std::string chineseDictionary;
    std::string chineseIndex;

    std::vector<DictionarySource> sources() const;
};

struct DictionaryGeneration
{
    std::string name;
    std::shared_ptr<const DictionaryRepository> repository;
};

/** 四份词库文件写入同一暂存目录，校验成功后才切换 CURRENT。 */
class DictionaryStore
{
public:
    using Writer = std::function<void(const DictionaryFiles &)>;

    static DictionaryGeneration publish(
        const std::string &root, const Writer &writeFiles,
        const std::vector<std::string> &buildInputs);
    static DictionaryGeneration loadCurrent(const std::string &root);
    static DictionaryGeneration activate(const std::string &root,
                                         const std::string &generation);
};
