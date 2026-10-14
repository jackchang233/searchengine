#include "webpage/PageProcessor.h"
#include "webpage/TextAnalyzer.h"
#include "Utf8Utils.h"

#include <simhash/Simhasher.hpp>
#include <tinyxml2.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <unistd.h>

namespace {

std::string resourcePath(const std::string &directory,
                         const std::string &name)
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

void appendText(const tinyxml2::XMLNode *node, std::string &text)
{
    for (const tinyxml2::XMLNode *child = node->FirstChild(); child;
         child = child->NextSibling()) {
        const tinyxml2::XMLText *value = child->ToText();
        if (value) {
            text += value->Value();
        } else if (child->ToElement()) {
            appendText(child, text);
        }
    }
}

std::string elementText(const tinyxml2::XMLElement *element)
{
    if (!element) {
        return std::string();
    }
    std::string result;
    appendText(element, result);
    return result;
}

void validateUtf8(const std::string &text, const std::string &field,
                  const std::string &filename, int line)
{
    if (!Utf8Utils::isValid(text)) {
        throw std::runtime_error(
            "invalid UTF-8 in " + field + " at " + filename + ":" +
            std::to_string(line));
    }
}

std::string serializeDocument(const PageProcessor::Document &document)
{
    tinyxml2::XMLDocument output;
    tinyxml2::XMLElement *root = output.NewElement("doc");
    output.InsertEndChild(root);

    tinyxml2::XMLElement *id = output.NewElement("docid");
    id->SetText(document.id);
    root->InsertEndChild(id);

    tinyxml2::XMLElement *link = output.NewElement("link");
    link->SetText(document.link.c_str());
    root->InsertEndChild(link);

    tinyxml2::XMLElement *title = output.NewElement("title");
    title->SetText(document.title.c_str());
    root->InsertEndChild(title);

    tinyxml2::XMLElement *content = output.NewElement("content");
    content->SetText(document.content.c_str());
    root->InsertEndChild(content);

    tinyxml2::XMLPrinter printer(nullptr, true);
    output.Print(&printer);
    return std::string(printer.CStr(), printer.CStrSize() - 1) + "\n";
}

/** 为输出创建同目录临时文件，异常时清理。 */
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

    ~TemporaryFile() { ::remove(_path.c_str()); }

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

class PageProcessor::Impl
{
public:
    Impl(const std::string &dictionaryDirectory, std::size_t topN,
         unsigned short hammingThreshold)
        : _topN(topN), _hammingThreshold(hammingThreshold)
    {
        if (_topN == 0) {
            throw std::invalid_argument("SimHash topN must be greater than zero");
        }
        if (_hammingThreshold > 64) {
            throw std::invalid_argument("SimHash Hamming threshold must not exceed 64");
        }

        const std::string directory = dictionaryDirectory.empty()
            ? PageProcessor::defaultDictionaryDirectory()
            : dictionaryDirectory;
        const std::string dictionary = resourcePath(directory, "jieba.dict.utf8");
        const std::string model = resourcePath(directory, "hmm_model.utf8");
        const std::string userDictionary = resourcePath(directory, "user.dict.utf8");
        const std::string idf = resourcePath(directory, "idf.utf8");
        const std::string stopWords = resourcePath(directory, "stop_words.utf8");
        requireReadable(dictionary);
        requireReadable(model);
        requireReadable(userDictionary);
        requireReadable(idf);
        requireReadable(stopWords);
        _hasher.reset(new simhash::Simhasher(dictionary, model, idf, stopWords));
        _analyzer.reset(new TextAnalyzer(directory));
    }

    std::uint64_t hash(const std::string &content) const
    {
        std::uint64_t result = 0;
        if (!_hasher->make(content, _topN, result)) {
            throw std::runtime_error("failed to compute document SimHash");
        }
        return result;
    }

    bool isDuplicate(std::uint64_t value,
                     const std::vector<std::uint64_t> &accepted) const
    {
        for (std::uint64_t existing : accepted) {
            if (simhash::Simhasher::isEqual(
                    value, existing, _hammingThreshold)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> tokenize(const std::string &title,
                                      const std::string &content) const
    {
        return _analyzer->tokenize(title, content);
    }

private:
    std::unique_ptr<simhash::Simhasher> _hasher;
    std::unique_ptr<TextAnalyzer> _analyzer;
    std::size_t _topN;
    unsigned short _hammingThreshold;
};

PageProcessor::PageProcessor(const std::string &jiebaDictionaryDirectory,
                             std::size_t topN,
                             unsigned short hammingThreshold)
    : _impl(new Impl(jiebaDictionaryDirectory, topN, hammingThreshold))
{
}

PageProcessor::~PageProcessor() = default;

namespace {

template <typename Implementation>
void processItem(const tinyxml2::XMLElement *item,
                 const std::string &filename,
                 Implementation &implementation,
                 std::vector<std::uint64_t> &hashes,
                 std::vector<PageProcessor::Document> &documents,
                 PageProcessor::Statistics &statistics)
{
    const tinyxml2::XMLElement *body = item->FirstChildElement("content");
    if (!body) {
        body = item->FirstChildElement("description");
    }
    if (!body) {
        ++statistics.ignored;
        return;
    }

    PageProcessor::Document document;
    document.id = 0;
    document.link = elementText(item->FirstChildElement("link"));
    document.title = elementText(item->FirstChildElement("title"));
    document.content = elementText(body);
    validateUtf8(document.link, "link", filename, item->GetLineNum());
    validateUtf8(document.title, "title", filename, item->GetLineNum());
    validateUtf8(document.content, "content", filename, body->GetLineNum());
    ++statistics.extracted;

    const std::uint64_t value = implementation.hash(document.content);
    if (implementation.isDuplicate(value, hashes)) {
        ++statistics.duplicates;
        return;
    }

    document.id = static_cast<std::uint64_t>(documents.size()) + 1;
    hashes.push_back(value);
    documents.push_back(std::move(document));
}

template <typename Implementation>
void visitElements(const tinyxml2::XMLNode *node,
                   const std::string &filename,
                   Implementation &implementation,
                   std::vector<std::uint64_t> &hashes,
                   std::vector<PageProcessor::Document> &documents,
                   PageProcessor::Statistics &statistics)
{
    for (const tinyxml2::XMLNode *child = node->FirstChild(); child;
         child = child->NextSibling()) {
        const tinyxml2::XMLElement *element = child->ToElement();
        if (!element) {
            continue;
        }
        if (std::string(element->Name()) == "item") {
            processItem(element, filename, implementation, hashes,
                        documents, statistics);
        } else {
            visitElements(element, filename, implementation, hashes,
                          documents, statistics);
        }
    }
}

} // namespace

void PageProcessor::process(const std::vector<std::string> &xmlFiles)
{
    if (xmlFiles.empty()) {
        throw std::invalid_argument("XML corpus file list must not be empty");
    }

    std::vector<Document> documents;
    std::vector<std::uint64_t> hashes;
    Statistics statistics{0, 0, 0};
    for (const std::string &filename : xmlFiles) {
        tinyxml2::XMLDocument input;
        const tinyxml2::XMLError error = input.LoadFile(filename.c_str());
        if (error != tinyxml2::XML_SUCCESS) {
            throw std::runtime_error(
                "cannot parse XML " + filename + ":" +
                std::to_string(input.ErrorLineNum()) + ": " +
                (input.ErrorStr() ? input.ErrorStr() : "unknown XML error"));
        }
        visitElements(&input, filename, *_impl, hashes, documents, statistics);
    }

    _documents.swap(documents);
    _statistics = statistics;
    _invertedIndex.clear();
    _ready = true;
    _indexReady = false;
}

void PageProcessor::store(const std::string &pageLibraryFile,
                          const std::string &offsetLibraryFile) const
{
    if (!_ready) {
        throw std::logic_error("process XML corpus before storing page library");
    }
    if (pageLibraryFile.empty() || offsetLibraryFile.empty() ||
        pageLibraryFile == offsetLibraryFile) {
        throw std::invalid_argument(
            "page library and offset library need distinct nonempty paths");
    }

    TemporaryFile pageTemporary(pageLibraryFile);
    TemporaryFile offsetTemporary(offsetLibraryFile);
    std::ofstream pages(pageTemporary.path().c_str(), std::ios::binary);
    std::ofstream offsets(offsetTemporary.path().c_str(), std::ios::binary);
    if (!pages) {
        throw std::runtime_error("cannot create page library: " + pageLibraryFile);
    }
    if (!offsets) {
        throw std::runtime_error("cannot create offset library: " + offsetLibraryFile);
    }

    std::uint64_t offset = 0;
    for (const Document &document : _documents) {
        const std::string page = serializeDocument(document);
        if (page.size() > std::numeric_limits<std::uint64_t>::max() - offset) {
            throw std::overflow_error("page library offset overflow");
        }
        pages.write(page.data(), static_cast<std::streamsize>(page.size()));
        offsets << document.id << ' ' << offset << ' ' << page.size() << '\n';
        offset += static_cast<std::uint64_t>(page.size());
    }

    finishOutput(pages, pageLibraryFile);
    finishOutput(offsets, offsetLibraryFile);
    pageTemporary.publish(pageLibraryFile);
    offsetTemporary.publish(offsetLibraryFile);
}

void PageProcessor::buildInvertedIndex(
    const std::vector<std::string> &stopWordFiles)
{
    if (!_ready) {
        throw std::logic_error("process XML corpus before building inverted index");
    }

    const std::set<std::string> stopWords =
        TextAnalyzer::loadStopWords(stopWordFiles);

    using Frequencies = std::map<std::string, std::size_t>;
    std::vector<Frequencies> documentFrequencies;
    documentFrequencies.reserve(_documents.size());
    std::map<std::string, std::size_t> documentCounts;

    for (const Document &document : _documents) {
        Frequencies frequencies;
        for (const std::string &word :
             _impl->tokenize(document.title, document.content)) {
            if (stopWords.find(word) == stopWords.end()) {
                ++frequencies[word];
            }
        }
        for (const auto &entry : frequencies) {
            ++documentCounts[entry.first];
        }
        documentFrequencies.push_back(std::move(frequencies));
    }

    InvertedIndex index;
    const double documentTotal = static_cast<double>(_documents.size());
    for (std::size_t position = 0; position < _documents.size(); ++position) {
        std::map<std::string, double> weights;
        double squaredLength = 0.0;
        for (const auto &entry : documentFrequencies[position]) {
            const std::size_t count = documentCounts.at(entry.first);
            const double inverseDocumentFrequency =
                std::log(documentTotal / static_cast<double>(count)) /
                std::log(2.0);
            const double weight =
                static_cast<double>(entry.second) * inverseDocumentFrequency;
            if (weight > 0.0) {
                weights[entry.first] = weight;
                squaredLength += weight * weight;
            }
        }

        if (squaredLength == 0.0) {
            continue;
        }
        const double length = std::sqrt(squaredLength);
        for (const auto &entry : weights) {
            index[entry.first].push_back(
                Posting(_documents[position].id, entry.second / length));
        }
    }

    _invertedIndex.swap(index);
    _indexReady = true;
}

void PageProcessor::storeInvertedIndex(
    const std::string &invertedIndexFile) const
{
    if (!_indexReady) {
        throw std::logic_error(
            "build inverted index before storing inverted index");
    }
    if (invertedIndexFile.empty()) {
        throw std::invalid_argument("inverted-index path must not be empty");
    }

    TemporaryFile temporary(invertedIndexFile);
    std::ofstream output(temporary.path().c_str(), std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "cannot create inverted index: " + invertedIndexFile);
    }
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    for (const auto &term : _invertedIndex) {
        output << term.first;
        for (const Posting &posting : term.second) {
            output << ' ' << posting.first << ' ' << posting.second;
        }
        output << '\n';
    }
    finishOutput(output, invertedIndexFile);
    temporary.publish(invertedIndexFile);
}

const std::vector<PageProcessor::Document> &PageProcessor::documents() const
{
    return _documents;
}

const PageProcessor::Statistics &PageProcessor::statistics() const
{
    return _statistics;
}

const PageProcessor::InvertedIndex &PageProcessor::invertedIndex() const
{
    return _invertedIndex;
}

std::string PageProcessor::defaultDictionaryDirectory()
{
    return SEARCHENGINE_JIEBA_DICT_DIR;
}
