#include "corpus/ChineseKeywordProcessor.h"
#include "corpus/ChineseTokenizer.h"
#include "DirectoryScanner.h"
#include "Utf8Utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Action>
void expectException(Action action, const std::string &message)
{
    try {
        action();
    } catch (const Exception &) {
        return;
    }
    throw std::runtime_error(message);
}

class TestDirectory
{
public:
    TestDirectory()
    {
        char pattern[] = "/tmp/chinese-keywords-test.XXXXXX";
        char *directory = ::mkdtemp(pattern);
        if (!directory) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        _root = directory;
    }

    ~TestDirectory()
    {
        for (const std::string &file : _files) {
            ::unlink(file.c_str());
        }
        ::rmdir(_root.c_str());
    }

    std::string file(const std::string &name)
    {
        const std::string path = _root + "/" + name;
        _files.push_back(path);
        return path;
    }

    const std::string &root() const { return _root; }

private:
    std::string _root;
    std::vector<std::string> _files;
};

void writeFile(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::binary);
    output << content;
    output.close();
    require(static_cast<bool>(output), "cannot write test file " + path);
}

std::string readFile(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    require(static_cast<bool>(input), "cannot read test file " + path);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void testUtf8()
{
    require(Utf8Utils::isValid(u8"A中国\U00020000\U0001F600"),
            "valid mixed UTF-8 rejected");
    require(Utf8Utils::splitCharacters(u8"A中国\U00020000\U0001F600") ==
                std::vector<std::string>({"A", "中", "国", u8"\U00020000",
                                          u8"\U0001F600"}),
            "UTF-8 splitting must preserve complete 1/3/4-byte characters");
    require(Utf8Utils::toCodePoints("中国") == U"中国",
            "code points must not be UTF-8 bytes");
    require(Utf8Utils::splitCharacters("").empty(), "empty text needs no characters");
    require(Utf8Utils::isHan(U'中') && Utf8Utils::isHan(U'\U00020000') &&
                !Utf8Utils::isHan(U'，') && !Utf8Utils::isHan(U'A'),
            "Han classification failed");

    const std::string invalid("\xF0\x28\x8C\x28", 4);
    require(!Utf8Utils::isValid(invalid), "invalid UTF-8 accepted");
    expectException<std::invalid_argument>([&] {
        Utf8Utils::splitCharacters(invalid);
    }, "invalid UTF-8 splitting did not throw");
}

void testProcessor()
{
    TestDirectory temporary;
    // 复用安装的基础模型，自定义少量词，固定分词预期且覆盖重复字符/混合词。
    for (const std::string &name :
         {"jieba.dict.utf8", "hmm_model.utf8", "idf.utf8", "stop_words.utf8"}) {
        const std::string source =
            ChineseTokenizer::defaultDictionaryDirectory() + "/" + name;
        const std::string destination = temporary.file(name);
        if (::symlink(source.c_str(), destination.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "symlink");
        }
    }
    writeFile(temporary.file("user.dict.utf8"),
              "人人网 1000000 nz\n中国 1000000 ns\n计算机 1000000 n\n"
              "c语言 1000000 n\n爱 1000000 v\n");

    ChineseTokenizer tokenizer(temporary.root());
    std::vector<std::string> words;
    tokenizer.cut("我来到北京清华大学", words);
    require(words == std::vector<std::string>({"我", "来到", "北京", "清华大学"}),
            "Mix segmentation differs from expected tokens");
    tokenizer.cut("", words);
    require(words.empty(), "tokenizer retained words from a previous call");
    expectException<std::invalid_argument>([&] {
        tokenizer.cut(std::string("\xC3", 1), words);
    }, "tokenizer accepted truncated UTF-8");
    expectException<std::runtime_error>([&] {
        ChineseTokenizer missing(temporary.root() + "/missing-models");
    }, "missing tokenizer resource was not reported");

    const std::string first = temporary.file("one.txt");
    const std::string second = temporary.file("two.txt");
    const std::string stops = temporary.file("stop.txt");
    const std::string dictionaryFile = temporary.file("dictionary.dat");
    const std::string indexFile = temporary.file("index.dat");
    writeFile(first, "\xEF\xBB\xBF我们 爱 中国 中国，人人网。\r\n"
                     "计算机 123，HELLO！ C语言\t３４。\n");
    writeFile(second, "中国\n计算机 的 人人网 爱");
    writeFile(stops, "\xEF\xBB\xBF 我们 \r\n的\r\n \t\r\n");

    ChineseKeywordProcessor processor(temporary.root());
    expectException<std::logic_error>([&] {
        processor.store(dictionaryFile, indexFile);
    }, "storing before process must fail");
    processor.process({first, second}, stops);
    const ChineseKeywordProcessor::Dictionary expected = {
        {"c语言", 1}, {"中国", 3}, {"人人网", 2}, {"爱", 2}, {"计算机", 2}
    };
    require(processor.dictionary() == expected,
            "word counts, stop-word filtering or mixed-word normalization failed");
    require(processor.stopWords() == std::set<std::string>({"我们", "的"}),
            "BOM/CRLF/whitespace stop-word normalization failed");
    const ChineseKeywordProcessor::Index expectedIndex = {
        {"c", {0}}, {"中", {1}}, {"人", {2}}, {"国", {1}}, {"机", {4}},
        {"爱", {3}}, {"算", {4}}, {"网", {2}}, {"言", {0}}, {"计", {4}},
        {"语", {0}}
    };
    require(processor.index() == expectedIndex,
            "character index or deduplication of repeated Han characters failed");

    processor.store(dictionaryFile, indexFile);
    const std::string savedDictionary = readFile(dictionaryFile);
    const std::string savedIndex = readFile(indexFile);
    require(savedDictionary == "c语言 1\n中国 3\n人人网 2\n爱 2\n计算机 2\n",
            "dictionary serialization failed");
    require(savedIndex.find("人 2\n") != std::string::npos,
            "index serialization failed");

    // 文件顺序不改变产物；复用处理器时不重复累加上一次的结果。
    processor.process({second, first}, stops);
    processor.store(dictionaryFile, indexFile);
    require(readFile(dictionaryFile) == savedDictionary &&
                readFile(indexFile) == savedIndex,
            "reprocessing must be deterministic and reset old counts");

    expectException<std::invalid_argument>([&] {
        processor.process({}, stops);
    }, "empty corpus list was not rejected");
    expectException<std::runtime_error>([&] {
        processor.process({first}, temporary.root() + "/missing-stop-words");
    }, "missing stop words were not rejected");
    expectException<std::runtime_error>([&] {
        processor.process({first, temporary.root() + "/missing-corpus"}, stops);
    }, "missing corpus was not rejected");

    const std::string badCorpus = temporary.file("invalid.txt");
    writeFile(badCorpus, "中国\n" + std::string("\xC3", 1));
    bool encodingLocationReported = false;
    try {
        processor.process({badCorpus}, stops);
    } catch (const std::runtime_error &error) {
        encodingLocationReported =
            std::string(error.what()).find(badCorpus + ":2") != std::string::npos;
    }
    require(encodingLocationReported, "encoding failure must report file and line");
    const std::string badStops = temporary.file("invalid-stops.txt");
    writeFile(badStops, std::string("\xFF", 1));
    expectException<std::runtime_error>([&] {
        processor.process({first}, badStops);
    }, "invalid stop-word encoding was not rejected");
    require(processor.dictionary() == expected && processor.index() == expectedIndex,
            "processing failures changed the previous successful result");

    expectException<std::invalid_argument>([&] {
        processor.store(dictionaryFile, dictionaryFile);
    }, "identical dictionary and index paths were not rejected");
    expectException<std::system_error>([&] {
        processor.store(dictionaryFile, temporary.root() + "/missing/index.dat");
    }, "unwritable output directory was not reported");
    require(readFile(dictionaryFile) == savedDictionary &&
                readFile(indexFile) == savedIndex,
            "failed temporary-file creation overwrote previous outputs");
    for (const std::string &path : DirectoryScanner::scan(temporary.root())) {
        require(path.find(".tmp.") == std::string::npos,
                "temporary output was not cleaned up");
    }

    const std::string empty = temporary.file("empty.txt");
    writeFile(empty, "");
    processor.process({empty}, stops);
    processor.store(dictionaryFile, indexFile);
    require(processor.dictionary().empty() && processor.index().empty() &&
                readFile(dictionaryFile).empty() && readFile(indexFile).empty(),
            "empty corpus retained old words");
}

} // namespace

int main()
{
    try {
        testUtf8();
        testProcessor();
        std::cout << "Chinese tokenizer, dictionary and UTF-8 index tests passed\n";
    } catch (const std::exception &error) {
        std::cerr << "Chinese keyword test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
