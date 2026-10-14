#include "corpus/KeywordProcessor.h"

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

namespace {

bool isAsciiLetter(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

char toAsciiLower(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

template <typename Consumer>
void consumeEnglishTokens(std::istream &input, Consumer consume)
{
    std::string word;
    char character;
    while (input.get(character)) {
        if (isAsciiLetter(character)) {
            word.push_back(toAsciiLower(character));
        } else if (!word.empty()) {
            consume(word);
            word.clear();
        }
    }

    if (!word.empty()) {
        consume(word);
    }
}

void replaceFile(const std::string &temporaryFile,
                 const std::string &destinationFile)
{
    if (::rename(temporaryFile.c_str(), destinationFile.c_str()) < 0) {
        const int error = errno;
        ::remove(temporaryFile.c_str());
        throw std::system_error(
            error, std::generic_category(),
            "rename " + temporaryFile + " to " + destinationFile);
    }
}

} // namespace

void KeywordProcessor::processEnglish(
    const std::vector<std::string> &corpusFiles,
    const std::string &stopWordsFile)
{
    if (corpusFiles.empty()) {
        throw std::invalid_argument("English corpus file list must not be empty");
    }

    _englishStopWords.clear();
    _englishDictionary.clear();
    _englishIndex.clear();

    loadEnglishStopWords(stopWordsFile);

    std::map<std::string, std::uint64_t> frequencies;
    for (const std::string &filename : corpusFiles) {
        countEnglishWords(filename, frequencies);
    }

    _englishDictionary.reserve(frequencies.size());
    for (const auto &entry : frequencies) {
        _englishDictionary.push_back(entry);
    }
    buildEnglishIndex();
}

void KeywordProcessor::loadEnglishStopWords(const std::string &filename)
{
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open English stop words: " + filename);
    }

    consumeEnglishTokens(input, [this](const std::string &word) {
        _englishStopWords.insert(word);
    });

    if (input.bad()) {
        throw std::runtime_error("failed to read English stop words: " + filename);
    }
}

void KeywordProcessor::countEnglishWords(
    const std::string &filename,
    std::map<std::string, std::uint64_t> &frequencies) const
{
    std::ifstream input(filename.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open English corpus: " + filename);
    }

    consumeEnglishTokens(input,
        [this, &frequencies](const std::string &word) {
            if (_englishStopWords.count(word) != 0) {
                return;
            }

            std::uint64_t &frequency = frequencies[word];
            if (frequency == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("English word frequency overflow: " + word);
            }
            ++frequency;
        });

    if (input.bad()) {
        throw std::runtime_error("failed to read English corpus: " + filename);
    }
}

void KeywordProcessor::buildEnglishIndex()
{
    for (std::size_t wordId = 0;
         wordId < _englishDictionary.size();
         ++wordId) {
        bool indexed[26] = {false};
        const std::string &word = _englishDictionary[wordId].first;

        for (char character : word) {
            const std::size_t position =
                static_cast<std::size_t>(character - 'a');
            if (!indexed[position]) {
                _englishIndex[character].push_back(wordId);
                indexed[position] = true;
            }
        }
    }
}

void KeywordProcessor::storeEnglish(
    const std::string &dictionaryFile,
    const std::string &indexFile) const
{
    const std::string dictionaryTemporary = dictionaryFile + ".tmp";
    const std::string indexTemporary = indexFile + ".tmp";

    {
        std::ofstream output(dictionaryTemporary.c_str(),
                             std::ios::out | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "cannot create English dictionary: " + dictionaryTemporary);
        }
        for (const WordFrequency &entry : _englishDictionary) {
            output << entry.first << ' ' << entry.second << '\n';
        }
        if (!output) {
            throw std::runtime_error(
                "failed to write English dictionary: " + dictionaryTemporary);
        }
    }

    {
        std::ofstream output(indexTemporary.c_str(),
                             std::ios::out | std::ios::trunc);
        if (!output) {
            ::remove(dictionaryTemporary.c_str());
            throw std::runtime_error(
                "cannot create English index: " + indexTemporary);
        }
        for (const auto &entry : _englishIndex) {
            output << entry.first;
            for (std::size_t wordId : entry.second) {
                output << ' ' << wordId;
            }
            output << '\n';
        }
        if (!output) {
            ::remove(dictionaryTemporary.c_str());
            ::remove(indexTemporary.c_str());
            throw std::runtime_error(
                "failed to write English index: " + indexTemporary);
        }
    }

    try {
        replaceFile(dictionaryTemporary, dictionaryFile);
        replaceFile(indexTemporary, indexFile);
    } catch (...) {
        ::remove(dictionaryTemporary.c_str());
        ::remove(indexTemporary.c_str());
        throw;
    }
}

const KeywordProcessor::Dictionary &
KeywordProcessor::englishDictionary() const
{
    return _englishDictionary;
}

const KeywordProcessor::Index &KeywordProcessor::englishIndex() const
{
    return _englishIndex;
}

const std::set<std::string> &KeywordProcessor::englishStopWords() const
{
    return _englishStopWords;
}
