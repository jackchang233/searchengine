#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct WordId
{
    std::size_t sourceId;
    std::size_t wordId;

    bool operator<(const WordId &other) const;
    bool operator==(const WordId &other) const;
};

struct WordEntry
{
    std::string word;
    std::u32string codePoints;
    std::uint64_t frequency;
};

struct DictionarySource
{
    std::string name;
    std::string dictionaryPath;
    std::string indexPath;
};

class DictionaryRepository
{
public:
    static std::shared_ptr<const DictionaryRepository>
    load(const std::vector<DictionarySource> &sources);

    std::size_t sourceCount() const;
    std::size_t wordCount(std::size_t sourceId) const;
    const WordEntry &word(const WordId &id) const;
    std::vector<WordId>
    findCandidates(const std::vector<std::string> &characters) const;

private:
    struct Snapshot
    {
        std::string name;
        std::vector<WordEntry> words;
        std::unordered_map<std::string, std::vector<std::size_t>> index;
    };

    DictionaryRepository() = default;
    std::vector<Snapshot> _sources;
};
