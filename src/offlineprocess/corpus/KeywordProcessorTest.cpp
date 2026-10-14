#include "corpus/KeywordProcessor.h"

#include <cassert>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

void writeFile(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test file: " + path);
    }
    output << content;
}

std::string readFile(const std::string &path)
{
    std::ifstream input(path.c_str());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    char pathTemplate[] = "/tmp/keyword-processor-test.XXXXXX";
    char *createdDirectory = ::mkdtemp(pathTemplate);
    if (createdDirectory == nullptr) {
        throw std::system_error(errno, std::generic_category(), "mkdtemp");
    }

    const std::string root(createdDirectory);
    const std::string corpusOne = root + "/one.txt";
    const std::string corpusTwo = root + "/two.txt";
    const std::string stopWords = root + "/stop.txt";
    const std::string dictionary = root + "/dictionary.dat";
    const std::string index = root + "/index.dat";

    writeFile(corpusOne, "Hello, HELLO! world42alpha. THE\n");
    writeFile(corpusTwo, "World hello; beta beta and\n");
    writeFile(stopWords, "the\nAND\n");

    KeywordProcessor processor;
    processor.processEnglish({corpusOne, corpusTwo}, stopWords);

    const KeywordProcessor::Dictionary expectedDictionary = {
        {"alpha", 1}, {"beta", 2}, {"hello", 3}, {"world", 2}
    };
    assert(processor.englishDictionary() == expectedDictionary);
    assert(processor.englishStopWords().count("the") == 1);
    assert(processor.englishStopWords().count("and") == 1);

    const KeywordProcessor::Index &builtIndex = processor.englishIndex();
    assert(builtIndex.at('a') == std::vector<std::size_t>({0, 1}));
    assert(builtIndex.at('l') == std::vector<std::size_t>({0, 2, 3}));
    assert(builtIndex.at('o') == std::vector<std::size_t>({2, 3}));

    processor.storeEnglish(dictionary, index);
    assert(readFile(dictionary) ==
           "alpha 1\nbeta 2\nhello 3\nworld 2\n");
    assert(readFile(index).find("a 0 1\n") != std::string::npos);

    bool missingCorpusRejected = false;
    try {
        processor.processEnglish({root + "/missing.txt"}, stopWords);
    } catch (const std::runtime_error &) {
        missingCorpusRejected = true;
    }
    assert(missingCorpusRejected);

    assert(::unlink(corpusOne.c_str()) == 0);
    assert(::unlink(corpusTwo.c_str()) == 0);
    assert(::unlink(stopWords.c_str()) == 0);
    assert(::unlink(dictionary.c_str()) == 0);
    assert(::unlink(index.c_str()) == 0);
    assert(::rmdir(root.c_str()) == 0);
    return 0;
}
