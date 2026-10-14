#include "webpage/PageProcessor.h"

#include <tinyxml2.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
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
        char pattern[] = "/tmp/page-processor-test.XXXXXX";
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

struct Offset
{
    std::uint64_t id;
    std::uint64_t begin;
    std::uint64_t size;
};

std::vector<Offset> readOffsets(const std::string &path)
{
    std::ifstream input(path.c_str());
    require(static_cast<bool>(input), "cannot read offsets");
    std::vector<Offset> result;
    Offset offset;
    while (input >> offset.id >> offset.begin >> offset.size) {
        result.push_back(offset);
    }
    require(input.eof(), "invalid offset format");
    return result;
}

PageProcessor::InvertedIndex readInvertedIndex(const std::string &path)
{
    std::ifstream input(path.c_str());
    require(static_cast<bool>(input), "cannot read inverted index");
    PageProcessor::InvertedIndex result;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string term;
        require(static_cast<bool>(fields >> term), "missing inverted-index term");
        std::uint64_t documentId = 0;
        double weight = 0.0;
        while (fields >> documentId) {
            require(static_cast<bool>(fields >> weight),
                    "posting has no matching weight");
            result[term].push_back(PageProcessor::Posting(documentId, weight));
        }
        require(fields.eof(), "invalid inverted-index posting");
    }
    require(input.eof(), "failed while reading inverted index");
    return result;
}

double postingWeight(const PageProcessor::InvertedIndex &index,
                     const std::string &term, std::uint64_t documentId)
{
    const auto termIterator = index.find(term);
    require(termIterator != index.end(), "missing term: " + term);
    for (const PageProcessor::Posting &posting : termIterator->second) {
        if (posting.first == documentId) {
            return posting.second;
        }
    }
    throw std::runtime_error("missing posting for term: " + term);
}

std::string childText(const tinyxml2::XMLElement *root, const char *name)
{
    const tinyxml2::XMLElement *element = root->FirstChildElement(name);
    require(element != nullptr, std::string("missing element ") + name);
    const char *text = element->GetText();
    return text ? text : "";
}

void verifyStoredPages(const std::string &pagesPath,
                       const std::string &offsetsPath,
                       const std::vector<PageProcessor::Document> &documents)
{
    const std::string pages = readFile(pagesPath);
    const std::vector<Offset> offsets = readOffsets(offsetsPath);
    require(offsets.size() == documents.size(), "offset count differs from pages");

    std::uint64_t expectedBegin = 0;
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        const Offset &offset = offsets[index];
        require(offset.id == index + 1, "document ids are not continuous");
        require(offset.begin == expectedBegin, "offsets are not contiguous");
        require(offset.begin + offset.size <= pages.size(), "offset is out of range");

        const std::string fragment = pages.substr(
            static_cast<std::size_t>(offset.begin),
            static_cast<std::size_t>(offset.size));
        tinyxml2::XMLDocument parsed;
        require(parsed.Parse(fragment.c_str(), fragment.size()) == tinyxml2::XML_SUCCESS,
                "stored doc fragment is not valid XML");
        const tinyxml2::XMLElement *root = parsed.FirstChildElement("doc");
        require(root != nullptr, "stored fragment has no doc root");
        require(childText(root, "docid") == std::to_string(index + 1),
                "stored id differs from offset id");
        require(childText(root, "link") == documents[index].link,
                "stored link changed");
        require(childText(root, "title") == documents[index].title,
                "stored title changed");
        require(childText(root, "content") == documents[index].content,
                "stored content changed");
        expectedBegin += offset.size;
    }
    require(expectedBegin == pages.size(), "offsets do not cover complete page library");
}

void testProcessingAndStorage()
{
    TestDirectory temporary;
    const std::string corpus = temporary.file("pages.xml");
    const std::string pages = temporary.file("pages.dat");
    const std::string offsets = temporary.file("offsets.dat");

    const std::string firstContent =
        "<p>量子计算芯片用于复杂科学模拟，研究团队公布了新的实验数据。</p>";
    const std::string fallbackContent =
        "农业灌溉系统正在改善旱区供水效率，农户开始使用新的节水设备。";
    writeFile(corpus,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<rss><channel>\n"
        "<item><title><![CDATA[A & B]]></title><link>https://example/a?x=1&amp;y=2</link>"
        "<description>这里不应被采用</description><content><![CDATA[" +
        firstContent + "]]></content></item>\n"
        "<item><title>回退正文</title><link>https://example/b</link>"
        "<description><![CDATA[" + fallbackContent + "]]></description></item>\n"
        "<item><title>缺正文</title><link>https://example/ignored</link></item>\n"
        "<item><title>重复正文</title><link>https://example/duplicate</link>"
        "<content><![CDATA[" + firstContent + "]]></content></item>\n"
        "<item><title>空 content 优先</title><description>不应回退</description>"
        "<content><![CDATA[]]></content></item>\n"
        "</channel></rss>\n");

    PageProcessor processor;
    expectException<std::logic_error>([&] {
        processor.buildInvertedIndex({});
    }, "building inverted index before process must fail");
    expectException<std::logic_error>([&] {
        processor.store(pages, offsets);
    }, "storing before process must fail");
    processor.process({corpus});

    const PageProcessor::Statistics &statistics = processor.statistics();
    require(statistics.extracted == 4, "eligible item count is wrong");
    require(statistics.ignored == 1, "missing-body item was not ignored");
    require(statistics.duplicates == 1, "exact duplicate was not removed");
    require(processor.documents().size() == 3, "retained document count is wrong");
    require(processor.documents()[0].title == "A & B", "title extraction failed");
    require(processor.documents()[0].link == "https://example/a?x=1&y=2",
            "entity decoding failed");
    require(processor.documents()[0].content == firstContent,
            "content must take priority over description");
    require(processor.documents()[1].content == fallbackContent,
            "description fallback failed");
    require(processor.documents()[2].content.empty(),
            "present empty content must not fall back to description");

    processor.store(pages, offsets);
    verifyStoredPages(pages, offsets, processor.documents());
    const std::string serialized = readFile(pages);
    require(serialized.find("A &amp; B") != std::string::npos &&
                serialized.find("&lt;p&gt;") != std::string::npos,
            "XML special characters were not escaped");

    expectException<std::invalid_argument>([&] {
        processor.store(pages, pages);
    }, "same page and offset path must be rejected");

    const std::string badXml = temporary.file("bad.xml");
    writeFile(badXml, "<rss><item><content>broken</rss>");
    expectException<std::runtime_error>([&] {
        processor.process({badXml});
    }, "malformed XML was accepted");
    require(processor.documents().size() == 3,
            "failed processing changed the previous successful result");
    expectException<std::invalid_argument>([&] {
        processor.process({});
    }, "empty XML file list was accepted");
}

void testTfIdfInvertedIndex()
{
    TestDirectory temporary;
    const std::string corpus = temporary.file("tfidf.xml");
    const std::string stopWords = temporary.file("stop.txt");
    const std::string indexFile = temporary.file("inverted.dat");
    writeFile(corpus,
        "<rss><channel>"
        "<item><title>headline</title><content><![CDATA[<p>alpha&nbsp; alpha beta</p>]]>"
        "</content></item>"
        "<item><title></title><content>beta gamma</content></item>"
        "<item><title></title><content>delta</content></item>"
        "</channel></rss>");
    writeFile(stopWords, "gamma\n");

    PageProcessor processor(std::string(), 20, 0);
    processor.process({corpus});
    expectException<std::logic_error>([&] {
        processor.storeInvertedIndex(indexFile);
    }, "storing an unbuilt inverted index must fail");
    processor.buildInvertedIndex({stopWords});

    const PageProcessor::InvertedIndex &index = processor.invertedIndex();
    require(index.find("gamma") == index.end(), "stop word entered the index");
    require(index.size() == 4, "unexpected number of indexed terms");
    require(index.at("alpha").size() == 1, "alpha document frequency is wrong");
    require(index.at("beta").size() == 2, "beta document frequency is wrong");
    require(index.at("delta").size() == 1, "delta document frequency is wrong");
    require(index.at("headline").size() == 1,
            "title term was not indexed");

    const double alphaRaw = 2.0 * (std::log(3.0) / std::log(2.0));
    const double betaRaw = std::log(1.5) / std::log(2.0);
    const double firstLength =
        std::sqrt(alphaRaw * alphaRaw + betaRaw * betaRaw +
                  std::pow(std::log(3.0) / std::log(2.0), 2.0));
    require(std::fabs(postingWeight(index, "alpha", 1) -
                      alphaRaw / firstLength) < 1e-12,
            "alpha TF-IDF weight is wrong");
    require(std::fabs(postingWeight(index, "beta", 1) -
                      betaRaw / firstLength) < 1e-12,
            "beta TF-IDF weight is wrong in document 1");
    require(std::fabs(postingWeight(index, "headline", 1) -
                      (std::log(3.0) / std::log(2.0)) / firstLength) < 1e-12,
            "title TF-IDF weight is wrong");
    require(std::fabs(postingWeight(index, "beta", 2) - 1.0) < 1e-12,
            "single nonzero document weight was not normalized");
    require(std::fabs(postingWeight(index, "delta", 3) - 1.0) < 1e-12,
            "delta document weight was not normalized");

    processor.storeInvertedIndex(indexFile);
    const PageProcessor::InvertedIndex stored = readInvertedIndex(indexFile);
    require(stored.size() == index.size(), "stored term count changed");
    for (const auto &term : index) {
        require(stored.at(term.first).size() == term.second.size(),
                "stored posting count changed");
        for (std::size_t i = 0; i < term.second.size(); ++i) {
            require(stored.at(term.first)[i].first == term.second[i].first &&
                        std::fabs(stored.at(term.first)[i].second -
                                  term.second[i].second) < 1e-15,
                    "stored posting changed");
        }
    }

    processor.process({corpus});
    require(processor.invertedIndex().empty(),
            "new document processing did not invalidate old index");
    expectException<std::logic_error>([&] {
        processor.storeInvertedIndex(indexFile);
    }, "invalidated inverted index was still storable");
}

} // namespace

int main()
{
    try {
        testProcessingAndStorage();
        testTfIdfInvertedIndex();
    } catch (const std::exception &error) {
        std::cerr << "PageProcessorTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
