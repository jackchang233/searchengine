#include "corpus/ChineseKeywordProcessor.h"
#include "Utf8Utils.h"

#include <utf8cpp/utf8.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace {

bool isAsciiLetter(char32_t character)
{
    return (character >= U'a' && character <= U'z') ||
           (character >= U'A' && character <= U'Z');
}

std::string prepareText(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (char32_t character : Utf8Utils::toCodePoints(text)) {
        if (Utf8Utils::isHan(character) || isAsciiLetter(character)) {
            if (character >= U'A' && character <= U'Z') {
                character = character - U'A' + U'a';
            }
            utf8::append(character, std::back_inserter(result));
        } else if (result.empty() || result.back() != ' ') {
            // 保留边界，避免“北京，上海”变成“北京上海”。
            result.push_back(' ');
        }
    }
    return result;
}

void validateEncoding(const std::string &text, const std::string &filename,
                      std::size_t lineNumber)
{
    if (!Utf8Utils::isValid(text)) {
        throw std::runtime_error("invalid UTF-8 in " + filename + ":" +
                                 std::to_string(lineNumber));
    }
}

std::string trimStopWord(std::string word, bool firstLine)
{
    if (firstLine && word.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        word.erase(0, 3);
    }
    const std::size_t begin = word.find_first_not_of(" \t\r");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::size_t end = word.find_last_not_of(" \t\r");
    word = word.substr(begin, end - begin + 1);
    for (char &character : word) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return word;
}

/** 为每次建库创建独立临时文件，异常时自动清理。 */
class TemporaryFile
{
public:
    explicit TemporaryFile(const std::string &destination)
    {
        const std::string pattern = destination + ".tmp.XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const int descriptor = ::mkstemp(name.data());
        if (descriptor < 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "create temporary file for " + destination);
        }
        _path = name.data();
        ::close(descriptor);
    }

    ~TemporaryFile()
    {
        ::remove(_path.c_str());
    }

    TemporaryFile(const TemporaryFile &) = delete;
    TemporaryFile &operator=(const TemporaryFile &) = delete;

    const std::string &path() const { return _path; }

    void publish(const std::string &destination) const
    {
        if (::rename(_path.c_str(), destination.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "rename " + _path + " to " + destination);
        }
    }

private:
    std::string _path;
};

void finishOutput(std::ofstream &output, const std::string &filename)
{
    output.close();
    if (!output) {
        throw std::runtime_error("failed to write " + filename);
    }
}

} // namespace

ChineseKeywordProcessor::ChineseKeywordProcessor(
    const std::string &jiebaDictionaryDirectory)
    : _tokenizer(jiebaDictionaryDirectory)
{
}

void ChineseKeywordProcessor::process(
    const std::vector<std::string> &corpusFiles,
    const std::string &stopWordsFile)
{
    if (corpusFiles.empty()) {
        throw std::invalid_argument("Chinese corpus file list must not be empty");
    }

    std::set<std::string> stops = loadStopWords(stopWordsFile);
    std::map<std::string, std::uint64_t> frequencies;
    for (const std::string &filename : corpusFiles) {
        countFile(filename, stops, frequencies);
    }

    Dictionary newDictionary(frequencies.begin(), frequencies.end());
    Index newIndex = buildIndex(newDictionary);
    _stopWords.swap(stops);
    _dictionary.swap(newDictionary);
    _index.swap(newIndex);
    _ready = true;
}

std::set<std::string> ChineseKeywordProcessor::loadStopWords(
    const std::string &filename)
{
    std::ifstream input(filename.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open Chinese stop words: " + filename);
    }

    std::set<std::string> stops;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        validateEncoding(line, filename, lineNumber);
        std::string word = trimStopWord(line, lineNumber == 1);
        if (!word.empty()) {
            stops.insert(word);
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read Chinese stop words: " + filename);
    }
    return stops;
}

void ChineseKeywordProcessor::countFile(
    const std::string &filename,
    const std::set<std::string> &stopWords,
    std::map<std::string, std::uint64_t> &frequencies) const
{
    std::ifstream input(filename.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open Chinese corpus: " + filename);
    }

    std::string line;
    std::vector<std::string> words;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        validateEncoding(line, filename, lineNumber);
        _tokenizer.cut(prepareText(line), words);
        for (const std::string &word : words) {
            if (!shouldKeep(word, stopWords)) {
                continue;
            }
            std::uint64_t &frequency = frequencies[word];
            if (frequency == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("Chinese word frequency overflow: " + word);
            }
            ++frequency;
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read Chinese corpus: " + filename);
    }
}

bool ChineseKeywordProcessor::shouldKeep(
    const std::string &word, const std::set<std::string> &stopWords)
{
    if (word.empty() || stopWords.count(word) != 0) {
        return false;
    }
    bool containsHan = false;
    for (char32_t character : Utf8Utils::toCodePoints(word)) {
        if (Utf8Utils::isHan(character)) {
            containsHan = true;
        } else if (!isAsciiLetter(character)) {
            return false;
        }
    }
    return containsHan;
}

ChineseKeywordProcessor::Index ChineseKeywordProcessor::buildIndex(
    const Dictionary &dictionary)
{
    Index result;
    for (std::size_t wordId = 0; wordId < dictionary.size(); ++wordId) {
        const auto characters = Utf8Utils::splitCharacters(dictionary[wordId].first);
        const std::set<std::string> uniqueCharacters(
            characters.begin(), characters.end());
        for (const std::string &character : uniqueCharacters) {
            result[character].push_back(wordId);
        }
    }
    return result;
}

void ChineseKeywordProcessor::store(
    const std::string &dictionaryFile, const std::string &indexFile) const
{
    if (!_ready) {
        throw std::logic_error("process Chinese corpus before storing results");
    }
    if (dictionaryFile.empty() || indexFile.empty() || dictionaryFile == indexFile) {
        throw std::invalid_argument("dictionary and index need distinct nonempty paths");
    }

    TemporaryFile dictionaryTemporary(dictionaryFile);
    TemporaryFile indexTemporary(indexFile);
    {
        std::ofstream output(dictionaryTemporary.path().c_str(), std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot create Chinese dictionary: " + dictionaryFile);
        }
        for (const WordFrequency &entry : _dictionary) {
            output << entry.first << ' ' << entry.second << '\n';
        }
        finishOutput(output, dictionaryFile);
    }
    {
        std::ofstream output(indexTemporary.path().c_str(), std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot create Chinese index: " + indexFile);
        }
        for (const auto &entry : _index) {
            output << entry.first;
            for (std::size_t wordId : entry.second) {
                output << ' ' << wordId;
            }
            output << '\n';
        }
        finishOutput(output, indexFile);
    }
    dictionaryTemporary.publish(dictionaryFile);
    indexTemporary.publish(indexFile);
}

const ChineseKeywordProcessor::Dictionary &
ChineseKeywordProcessor::dictionary() const
{
    return _dictionary;
}

const ChineseKeywordProcessor::Index &ChineseKeywordProcessor::index() const
{
    return _index;
}

const std::set<std::string> &ChineseKeywordProcessor::stopWords() const
{
    return _stopWords;
}
