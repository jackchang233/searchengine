#include "webpage/PageProcessor.h"
#include "webpage/WebIndexStore.h"

#include <dirent.h>
#include <cerrno>
#include <fstream>
#include <iostream>
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

template <typename Action>
void expectFailure(Action action, const std::string &message)
{
    try {
        action();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

void writeFile(const std::string &path, const std::string &content)
{
    std::ofstream output(path.c_str(), std::ios::binary);
    output << content;
    output.close();
    require(static_cast<bool>(output), "cannot write test file");
}

class TestDirectory
{
public:
    TestDirectory()
    {
        char pattern[] = "/tmp/web-index-store-test.XXXXXX";
        char *path = ::mkdtemp(pattern);
        if (!path) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        _root = path;
    }

    ~TestDirectory()
    {
        const std::string indexRoot = _root + "/index";
        const std::string generations = indexRoot + "/generations";
        DIR *directory = ::opendir(generations.c_str());
        if (directory) {
            while (struct dirent *entry = ::readdir(directory)) {
                const std::string name(entry->d_name);
                if (name == "." || name == "..") {
                    continue;
                }
                const std::string version = generations + "/" + name;
                for (const char *file : {"webpage.dat", "offset.dat",
                                         "inverted_index.dat", "manifest.json"}) {
                    ::unlink((version + "/" + file).c_str());
                }
                ::rmdir(version.c_str());
            }
            ::closedir(directory);
        }
        ::unlink((indexRoot + "/CURRENT").c_str());
        ::unlink((indexRoot + "/PUBLISH.lock").c_str());
        ::rmdir(generations.c_str());
        ::rmdir(indexRoot.c_str());
        ::unlink((_root + "/pages.xml").c_str());
        ::unlink((_root + "/stop.txt").c_str());
        ::rmdir(_root.c_str());
    }

    const std::string &root() const { return _root; }

private:
    std::string _root;
};

void testPublishingAndLoading()
{
    TestDirectory temporary;
    const std::string corpus = temporary.root() + "/pages.xml";
    const std::string stopWords = temporary.root() + "/stop.txt";
    const std::string indexRoot = temporary.root() + "/index";
    writeFile(corpus,
        "<rss><item><title>First</title><content>alpha alpha</content></item>"
        "<item><title>Second</title><content>beta beta</content></item>"
        "<item><title>Third</title><content>gamma gamma</content></item></rss>");
    writeFile(stopWords, "gamma\n");

    PageProcessor processor(std::string(), 20, 0);
    processor.process({corpus});
    processor.buildInvertedIndex({stopWords});
    WebIndexBuildInputs inputs;
    inputs.xmlFiles = {corpus};
    inputs.stopWordFiles = {stopWords};
    inputs.hammingThreshold = 0;

    WebIndexBuildInputs invalid = inputs;
    invalid.stopWordFiles = {temporary.root() + "/missing-stop.txt"};
    expectFailure([&] {
        WebIndexStore::publish(processor, indexRoot, invalid);
    }, "invalid build input was published");
    expectFailure([&] {
        WebIndexStore::loadCurrent(indexRoot);
    }, "CURRENT appeared after a failed initial publication");

    const WebIndexSnapshot first =
        WebIndexStore::publish(processor, indexRoot, inputs);
    const WebIndexSnapshot loadedFirst = WebIndexStore::loadCurrent(indexRoot);
    require(first.generation() == loadedFirst.generation(),
            "published generation differs from CURRENT");
    require(loadedFirst.offsets().size() == 3 &&
                loadedFirst.readDocument(1).content == "alpha alpha" &&
                loadedFirst.invertedIndex().count("alpha") == 1,
            "first generation cannot be read");
    expectFailure([&] { loadedFirst.readDocument(4); },
                  "out-of-range document ID was accepted");

    writeFile(corpus,
        "<rss><item><title>Changed</title><content>delta delta</content></item>"
        "<item><title>Second</title><content>beta beta</content></item>"
        "<item><title>Third</title><content>gamma gamma</content></item></rss>");
    processor.process({corpus});
    processor.buildInvertedIndex({stopWords});
    expectFailure([&] {
        WebIndexStore::publish(processor, indexRoot, invalid);
    }, "failed rebuild changed CURRENT");
    require(WebIndexStore::loadCurrent(indexRoot).generation() == first.generation(),
            "failed rebuild replaced the active generation");

    const WebIndexSnapshot second =
        WebIndexStore::publish(processor, indexRoot, inputs);
    require(second.generation() != first.generation() &&
                WebIndexStore::loadCurrent(indexRoot).generation() == second.generation() &&
                second.readDocument(1).content == "delta delta" &&
                first.readDocument(1).content == "alpha alpha",
            "switching generation changed an older snapshot");

    const std::string indexFile = indexRoot + "/generations/" +
        second.generation() + "/inverted_index.dat";
    std::ofstream corruption(indexFile.c_str(), std::ios::app | std::ios::binary);
    corruption << "corruption\n";
    corruption.close();
    expectFailure([&] { WebIndexStore::loadCurrent(indexRoot); },
                  "corrupted active generation was accepted");
    writeFile(indexRoot + "/CURRENT", first.generation() + "\n");
    require(WebIndexStore::loadCurrent(indexRoot).generation() == first.generation(),
            "previous generation cannot be restored");
    writeFile(indexRoot + "/CURRENT", "../outside\n");
    expectFailure([&] { WebIndexStore::loadCurrent(indexRoot); },
                  "unsafe generation name was accepted");
}

} // namespace

int main()
{
    try {
        testPublishingAndLoading();
    } catch (const std::exception &error) {
        std::cerr << "WebIndexStoreTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
