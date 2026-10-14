#include "DictionaryRepository.h"

#include "Utf8Utils.h"

#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::runtime_error invalidLine(const std::string &path, std::size_t line,
                               const std::string &reason)
{
    return std::runtime_error(path + ":" + std::to_string(line) + ": " + reason);
}

std::uint64_t parseDecimal(const std::string &value,
                           const std::string &path, std::size_t line,
                           bool positive)
{
    if (value.empty()) {
        throw invalidLine(path, line, "missing decimal number");
    }
    std::uint64_t result = 0;
    for (char character : value) {
        if (character < '0' || character > '9') {
            throw invalidLine(path, line, "invalid decimal number: " + value);
        }
        const unsigned digit = static_cast<unsigned>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            throw invalidLine(path, line, "decimal number overflow: " + value);
        }
        result = result * 10 + digit;
    }
    if (positive && result == 0) {
        throw invalidLine(path, line, "frequency must be positive");
    }
    return result;
}

bool isAsciiLetter(char32_t point)
{
    return point >= U'a' && point <= U'z';
}

void validateWord(const std::string &word, const std::string &name,
                  const std::string &path, std::size_t line,
                  const std::u32string &points)
{
    if (points.empty()) {
        throw invalidLine(path, line, "empty word");
    }
    bool hasHan = false;
    for (char32_t point : points) {
        if (Utf8Utils::isHan(point)) {
            hasHan = true;
        } else if (!isAsciiLetter(point)) {
            throw invalidLine(path, line, "word contains unsupported character: " + word);
        }
    }
    if ((name == "en" && hasHan) || (name == "cn" && !hasHan)) {
        throw invalidLine(path, line, "word does not match source: " + word);
    }
}

void loadDictionary(const DictionarySource &source,
                    std::vector<WordEntry> &words,
                    std::map<std::string, std::vector<std::size_t>> &expected)
{
    std::ifstream input(source.dictionaryPath.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open dictionary: " + source.dictionaryPath);
    }
    std::set<std::string> seen;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!Utf8Utils::isValid(line)) {
            throw invalidLine(source.dictionaryPath, lineNumber, "invalid UTF-8");
        }
        std::istringstream fields(line);
        std::string word;
        std::string frequency;
        std::string extra;
        if (!(fields >> word >> frequency) || fields >> extra) {
            throw invalidLine(source.dictionaryPath, lineNumber,
                              "expected word frequency");
        }
        if (!seen.insert(word).second) {
            throw invalidLine(source.dictionaryPath, lineNumber,
                              "duplicate word: " + word);
        }
        WordEntry entry;
        entry.word = word;
        entry.frequency = parseDecimal(frequency, source.dictionaryPath,
                                       lineNumber, true);
        entry.codePoints = Utf8Utils::toCodePoints(word);
        validateWord(word, source.name, source.dictionaryPath,
                     lineNumber, entry.codePoints);
        const std::size_t wordId = words.size();
        std::set<std::string> uniqueCharacters;
        for (const std::string &character : Utf8Utils::splitCharacters(word)) {
            uniqueCharacters.insert(character);
        }
        for (const std::string &character : uniqueCharacters) {
            expected[character].push_back(wordId);
        }
        words.push_back(std::move(entry));
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read dictionary: " + source.dictionaryPath);
    }
    if (words.empty()) {
        throw std::runtime_error("empty dictionary: " + source.dictionaryPath);
    }
}

void loadIndex(const DictionarySource &source,
               const std::map<std::string, std::vector<std::size_t>> &expected,
               std::unordered_map<std::string, std::vector<std::size_t>> &index)
{
    std::ifstream input(source.indexPath.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open index: " + source.indexPath);
    }
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!Utf8Utils::isValid(line)) {
            throw invalidLine(source.indexPath, lineNumber, "invalid UTF-8");
        }
        std::istringstream fields(line);
        std::string character;
        if (!(fields >> character)) {
            throw invalidLine(source.indexPath, lineNumber, "empty index line");
        }
        if (Utf8Utils::toCodePoints(character).size() != 1) {
            throw invalidLine(source.indexPath, lineNumber,
                              "index key must be one Unicode code point");
        }
        const auto wanted = expected.find(character);
        if (wanted == expected.end()) {
            throw invalidLine(source.indexPath, lineNumber,
                              "index character not found in dictionary: " + character);
        }
        if (index.count(character) != 0) {
            throw invalidLine(source.indexPath, lineNumber,
                              "duplicate index key: " + character);
        }
        std::vector<std::size_t> ids;
        std::string token;
        while (fields >> token) {
            const std::uint64_t parsed = parseDecimal(token, source.indexPath,
                                                       lineNumber, false);
            if (parsed > std::numeric_limits<std::size_t>::max()) {
                throw invalidLine(source.indexPath, lineNumber, "word id overflow");
            }
            const std::size_t id = static_cast<std::size_t>(parsed);
            if (!ids.empty() && id <= ids.back()) {
                throw invalidLine(source.indexPath, lineNumber,
                                  "word ids must be strictly increasing");
            }
            ids.push_back(id);
        }
        if (ids != wanted->second) {
            throw invalidLine(source.indexPath, lineNumber,
                              "index postings do not match dictionary: " + character);
        }
        index.emplace(character, std::move(ids));
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read index: " + source.indexPath);
    }
    if (index.size() != expected.size()) {
        for (const auto &entry : expected) {
            if (index.count(entry.first) == 0) {
                throw invalidLine(source.indexPath, lineNumber + 1,
                                  "missing character index key: " + entry.first);
            }
        }
    }
}

} // namespace

bool WordId::operator<(const WordId &other) const
{
    return sourceId < other.sourceId ||
           (sourceId == other.sourceId && wordId < other.wordId);
}

bool WordId::operator==(const WordId &other) const
{
    return sourceId == other.sourceId && wordId == other.wordId;
}

std::shared_ptr<const DictionaryRepository>
DictionaryRepository::load(const std::vector<DictionarySource> &sources)
{
    if (sources.empty()) {
        throw std::invalid_argument("at least one dictionary source is required");
    }
    std::set<std::string> names;
    std::set<std::string> paths;
    std::shared_ptr<DictionaryRepository> repository(new DictionaryRepository);
    for (const DictionarySource &source : sources) {
        if (source.name.empty() || source.dictionaryPath.empty() ||
            source.indexPath.empty() || !names.insert(source.name).second ||
            !paths.insert(source.dictionaryPath).second ||
            !paths.insert(source.indexPath).second) {
            throw std::invalid_argument("dictionary sources need unique names and paths");
        }
        Snapshot snapshot;
        snapshot.name = source.name;
        std::map<std::string, std::vector<std::size_t>> expected;
        loadDictionary(source, snapshot.words, expected);
        loadIndex(source, expected, snapshot.index);
        repository->_sources.push_back(std::move(snapshot));
    }
    return repository;
}

std::size_t DictionaryRepository::sourceCount() const
{
    return _sources.size();
}

std::size_t DictionaryRepository::wordCount(std::size_t sourceId) const
{
    return _sources.at(sourceId).words.size();
}

const WordEntry &DictionaryRepository::word(const WordId &id) const
{
    return _sources.at(id.sourceId).words.at(id.wordId);
}

std::vector<WordId> DictionaryRepository::findCandidates(
    const std::vector<std::string> &characters) const
{
    std::set<WordId> unique;
    for (std::size_t sourceId = 0; sourceId < _sources.size(); ++sourceId) {
        for (const std::string &character : characters) {
            const auto found = _sources[sourceId].index.find(character);
            if (found == _sources[sourceId].index.end()) {
                continue;
            }
            for (std::size_t wordId : found->second) {
                unique.insert(WordId{sourceId, wordId});
            }
        }
    }
    return std::vector<WordId>(unique.begin(), unique.end());
}
