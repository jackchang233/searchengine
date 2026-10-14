#include "webpage/TextAnalyzer.h"
#include "webpage/PageProcessor.h"
#include "Utf8Utils.h"

#include <cppjieba/Jieba.hpp>

#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace {

std::string resourcePath(const std::string &directory, const std::string &name)
{
    return directory + (directory.back() == '/' ? "" : "/") + name;
}

void requireReadable(const std::string &filename)
{
    std::ifstream input(filename.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open resource: " + filename);
    }
}

bool isAsciiLetter(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z');
}

char toAsciiLower(char character)
{
    return character >= 'A' && character <= 'Z'
        ? static_cast<char>(character - 'A' + 'a') : character;
}

void appendSpace(std::string &text)
{
    if (!text.empty() && text.back() != ' ') {
        text.push_back(' ');
    }
}

std::string normalizeIndexText(const std::string &text)
{
    std::string normalized;
    for (const std::string &character : Utf8Utils::splitCharacters(text)) {
        if (character.size() == 1 && isAsciiLetter(character[0])) {
            normalized.push_back(toAsciiLower(character[0]));
            continue;
        }
        const std::u32string codePoints = Utf8Utils::toCodePoints(character);
        if (!codePoints.empty() && Utf8Utils::isHan(codePoints.front())) {
            normalized += character;
        } else {
            appendSpace(normalized);
        }
    }
    return normalized;
}

std::string normalizeToken(const std::string &token)
{
    std::string normalized;
    for (const std::string &character : Utf8Utils::splitCharacters(token)) {
        if (character.size() == 1 && isAsciiLetter(character[0])) {
            normalized.push_back(toAsciiLower(character[0]));
            continue;
        }
        const std::u32string codePoints = Utf8Utils::toCodePoints(character);
        if (!codePoints.empty() && Utf8Utils::isHan(codePoints.front())) {
            normalized += character;
        } else {
            return std::string();
        }
    }
    return normalized;
}

} // namespace

class TextAnalyzer::Impl
{
public:
    explicit Impl(const std::string &directory)
    : tokenizer(resourcePath(directory, "jieba.dict.utf8"),
                resourcePath(directory, "hmm_model.utf8"),
                resourcePath(directory, "user.dict.utf8"),
                resourcePath(directory, "idf.utf8"),
                resourcePath(directory, "stop_words.utf8"))
    {
    }

    mutable std::mutex mutex;
    cppjieba::Jieba tokenizer;
};

TextAnalyzer::TextAnalyzer(const std::string &dictionaryDirectory)
{
    const std::string directory = dictionaryDirectory.empty()
        ? PageProcessor::defaultDictionaryDirectory() : dictionaryDirectory;
    for (const char *name : {"jieba.dict.utf8", "hmm_model.utf8",
                             "user.dict.utf8", "idf.utf8", "stop_words.utf8"}) {
        requireReadable(resourcePath(directory, name));
    }
    _impl.reset(new Impl(directory));
}

TextAnalyzer::~TextAnalyzer() = default;

std::string TextAnalyzer::visibleText(const std::string &html)
{
    std::string visible;
    bool inTag = false;
    char quote = '\0';
    for (std::size_t index = 0; index < html.size(); ++index) {
        const char character = html[index];
        if (inTag) {
            if (quote != '\0') {
                if (character == quote) quote = '\0';
            } else if (character == '\'' || character == '"') {
                quote = character;
            } else if (character == '>') {
                inTag = false;
                appendSpace(visible);
            }
            continue;
        }
        if (character == '<') {
            inTag = true;
            appendSpace(visible);
            continue;
        }
        if (character == '&') {
            const std::size_t end = html.find(';', index + 1);
            if (end != std::string::npos && end - index <= 16) {
                appendSpace(visible);
                index = end;
                continue;
            }
        }
        visible.push_back(character);
    }
    return visible;
}

std::vector<std::string> TextAnalyzer::tokenize(
    const std::string &title, const std::string &content) const
{
    const std::string text = normalizeIndexText(
        visibleText(title) + " " + visibleText(content));
    std::vector<std::string> rawWords;
    {
        std::lock_guard<std::mutex> guard(_impl->mutex);
        _impl->tokenizer.Cut(text, rawWords, true);
    }
    std::vector<std::string> words;
    words.reserve(rawWords.size());
    for (const std::string &rawWord : rawWords) {
        const std::string word = normalizeToken(rawWord);
        if (!word.empty()) words.push_back(word);
    }
    return words;
}

std::set<std::string> TextAnalyzer::loadStopWords(
    const std::vector<std::string> &files)
{
    std::set<std::string> stopWords;
    for (const std::string &filename : files) {
        if (filename.empty()) {
            throw std::invalid_argument("stop-word file path must not be empty");
        }
        std::ifstream input(filename.c_str(), std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open stop-word file: " + filename);
        }
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) {
                line.erase(0, 3);
            }
            if (!Utf8Utils::isValid(line)) {
                throw std::runtime_error("invalid UTF-8 in stop-word file: " + filename);
            }
            std::istringstream words(normalizeIndexText(line));
            std::string word;
            while (words >> word) {
                const std::string normalized = normalizeToken(word);
                if (!normalized.empty()) stopWords.insert(normalized);
            }
        }
        if (!input.eof()) {
            throw std::runtime_error("failed to read stop-word file: " + filename);
        }
    }
    return stopWords;
}
