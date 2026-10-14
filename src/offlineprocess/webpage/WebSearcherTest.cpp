#include "webpage/PageProcessor.h"
#include "webpage/WebIndexStore.h"
#include "webpage/WebSearcher.h"

#include <dirent.h>
#include <cerrno>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace {

void require(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}

class TemporaryIndex
{
public:
    TemporaryIndex()
    {
        char pattern[] = "/tmp/web-searcher-test.XXXXXX";
        char *path = ::mkdtemp(pattern);
        if (!path) throw std::system_error(errno, std::generic_category(),
                                           "mkdtemp");
        _root = path;
    }

    ~TemporaryIndex()
    {
        const std::string generations = _root + "/generations";
        DIR *directory = ::opendir(generations.c_str());
        if (directory) {
            while (struct dirent *entry = ::readdir(directory)) {
                const std::string name(entry->d_name);
                if (name == "." || name == "..") continue;
                const std::string version = generations + "/" + name;
                for (const char *file : {"webpage.dat", "offset.dat",
                                         "inverted_index.dat", "manifest.json"}) {
                    ::unlink((version + "/" + file).c_str());
                }
                ::rmdir(version.c_str());
            }
            ::closedir(directory);
        }
        ::unlink((_root + "/CURRENT").c_str());
        ::unlink((_root + "/PUBLISH.lock").c_str());
        ::rmdir(generations.c_str());
        ::rmdir(_root.c_str());
    }

    const std::string &root() const { return _root; }

private:
    std::string _root;
};

void testSearch()
{
    const std::string corpus = SEARCHENGINE_WEBSEARCH_FIXTURE_DIR "/pages.xml";
    const std::string stopWords = SEARCHENGINE_WEBSEARCH_FIXTURE_DIR "/stop.txt";
    TemporaryIndex temporary;
    PageProcessor processor(std::string(), 20, 0);
    processor.process({corpus});
    processor.buildInvertedIndex({stopWords});
    WebIndexBuildInputs inputs;
    inputs.xmlFiles = {corpus};
    inputs.stopWordFiles = {stopWords};
    inputs.hammingThreshold = 0;
    WebIndexStore::publish(processor, temporary.root(), inputs);
    std::shared_ptr<const WebIndexSnapshot> snapshot =
        std::make_shared<WebIndexSnapshot>(
            WebIndexStore::loadCurrent(temporary.root()));
    snapshot->validateAnalyzerInputs("", {stopWords});
    bool rejectedMismatch = false;
    try {
        snapshot->validateAnalyzerInputs(
            "", {SEARCHENGINE_WEBSEARCH_FIXTURE_DIR "/stop-changed.txt"});
    } catch (const std::runtime_error &) {
        rejectedMismatch = true;
    }
    require(rejectedMismatch, "changed stop words were not rejected");
    WebSearcher searcher(snapshot, "", {stopWords}, 64, 20);
    const WebSearchResult result = searcher.search("ALPHA beta", 2);
    require(result.candidateCount == 1 && result.hits.size() == 1 &&
                result.hits[0].documentId == 1 &&
                result.hits[0].score > 0.0 &&
                result.hits[0].url == "https://example.test/alpha" &&
                result.hits[0].snippet.find("<b>") == std::string::npos,
            "AND retrieval, scoring or snippet failed");
    const WebSearchResult unknown = searcher.search("alpha unknownword", 2);
    require(unknown.hits.empty() && unknown.candidateCount == 0,
            "unknown term should leave an empty intersection");
    bool rejected = false;
    try { searcher.search("the", 2); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "stop-word-only query was not rejected");
}

} // namespace

int main()
{
    try { testSearch(); }
    catch (const std::exception &error) {
        std::cerr << "WebSearcherTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
