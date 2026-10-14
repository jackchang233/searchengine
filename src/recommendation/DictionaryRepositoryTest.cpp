#include "Configuration.h"
#include "DictionaryRepository.h"
#include "DictionaryStore.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action>
void expectFailure(Action action, const std::string &fragment)
{
    try {
        action();
    } catch (const std::exception &error) {
        require(std::string(error.what()).find(fragment) != std::string::npos,
                "wrong failure reason: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected failure containing: " + fragment);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        char pattern[] = "/tmp/searchengine-dictionary-test-XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created) {
            throw std::runtime_error("mkdtemp failed");
        }
        _path = created;
    }

    ~TemporaryDirectory()
    {
        for (const std::string &filename : _files) {
            ::unlink(filename.c_str());
        }
        ::rmdir(_path.c_str());
    }

    std::string write(const std::string &name, const std::string &contents)
    {
        const std::string filename = _path + "/" + name;
        std::ofstream output(filename.c_str(), std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create test fixture: " + filename);
        }
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot write test fixture: " + filename);
        }
        _files.push_back(filename);
        return filename;
    }

    const std::string &path() const { return _path; }

private:
    std::string _path;
    std::vector<std::string> _files;
};

std::string configText()
{
    return "en_dict_path = en_dict.dat\n"
           "en_dict_index_path = en_index.dat\n"
           "cn_dict_path = cn_dict.dat\n"
           "cn_dict_index_path = cn_index.dat\n"
           "server_ip = 127.0.0.1\n"
           "server_port = 8888\n"
           "thread_num = 2\n"
           "buf_size = 4\n";
}

std::vector<DictionarySource> sources(const ServerOptions &options)
{
    return {{"en", options.enDictionaryPath, options.enIndexPath},
            {"cn", options.cnDictionaryPath, options.cnIndexPath}};
}

void testFixtures()
{
    TemporaryDirectory fixture;
    const std::string englishDictionary =
        fixture.write("en_dict.dat", "apple 2\nhello 3\n");
    const std::string englishIndex = fixture.write(
        "en_index.dat", "a 0\ne 0 1\nh 1\nl 0 1\no 1\np 0\n");
    fixture.write("cn_dict.dat", "中国 5\n中文 2\n");
    const std::string chineseIndex =
        fixture.write("cn_index.dat", "中 0 1\n国 0\n文 1\n");
    const std::string config = fixture.write("server.conf", configText());

    const ServerOptions options = Configuration::load(config);
    require(options.enDictionaryPath == englishDictionary &&
                options.cnIndexPath == chineseIndex && options.queueCapacity == 4 &&
                options.defaultTopK == 5,
            "configuration did not resolve paths or defaults");
    const std::shared_ptr<const DictionaryRepository> repository =
        DictionaryRepository::load(sources(options));
    require(repository->sourceCount() == 2 && repository->wordCount(0) == 2 &&
                repository->wordCount(1) == 2,
            "dictionary source counts are wrong");
    require(repository->word(WordId{0, 0}).word == "apple" &&
                repository->word(WordId{1, 0}).word == "中国" &&
                repository->word(WordId{1, 0}).codePoints.size() == 2,
            "independent source IDs or code points are wrong");
    const std::vector<WordId> candidates =
        repository->findCandidates({"中", "h", "中"});
    require(candidates == std::vector<WordId>({{0, 1}, {1, 0}, {1, 1}}),
            "candidate union or source-aware deduplication failed");

    char previous[4096];
    require(::getcwd(previous, sizeof(previous)) != nullptr,
            "cannot read current working directory");
    require(::chdir("/tmp") == 0, "cannot change working directory");
    try {
        const ServerOptions elsewhere = Configuration::load(config);
        require(elsewhere.enDictionaryPath == options.enDictionaryPath,
                "relative data path changed with working directory");
    } catch (...) {
        ::chdir(previous);
        throw;
    }
    require(::chdir(previous) == 0, "cannot restore working directory");

    expectFailure([&] {
        Configuration::load(config, {{"server_port", "65536"}});
    }, "server_port");
    expectFailure([&] {
        Configuration::load(config, {{"thread_num", "0"}});
    }, "thread_num");
    expectFailure([&] {
        Configuration::load(config, {{"buf_size", "184467440737095516160"}});
    }, "buf_size");
    expectFailure([&] {
        Configuration::load(config, {{"max_top_k", "2"}});
    }, "default_top_k exceeds");
    expectFailure([&] {
        Configuration::load(config, {{"en_dict_path", "cn_dict.dat"}});
    }, "distinct");
    expectFailure([&] {
        Configuration::load(config, {{"not_a_key", "1"}});
    }, "override");
    fixture.write("bad.conf", configText() + "server_port = 9999\n");
    expectFailure([&] {
        Configuration::load(fixture.path() + "/bad.conf");
    }, "duplicate key: server_port");
    fixture.write("bad.conf", "server_ip = 127.0.0.1\n");
    expectFailure([&] {
        Configuration::load(fixture.path() + "/bad.conf");
    }, "en_dict_path");

    const auto expectBadEnglishDictionary = [&](const std::string &contents,
                                                const std::string &reason) {
        fixture.write("en_dict.dat", contents);
        expectFailure([&] { DictionaryRepository::load(sources(options)); },
                      reason);
    };
    expectBadEnglishDictionary("apple 0\nhello 3\n", "frequency must be positive");
    expectBadEnglishDictionary("apple 2\napple 3\n", "duplicate word");
    expectBadEnglishDictionary("apple 18446744073709551616\n", "overflow");
    expectBadEnglishDictionary(std::string("bad\xFF 1\n"), "invalid UTF-8");
    fixture.write("en_dict.dat", "apple 2\nhello 3\n");

    const auto expectBadEnglishIndex = [&](const std::string &contents,
                                           const std::string &reason) {
        fixture.write("en_index.dat", contents);
        expectFailure([&] { DictionaryRepository::load(sources(options)); },
                      reason);
    };
    expectBadEnglishIndex("a 0\n", "missing character index key");
    expectBadEnglishIndex("a 2\n", "postings do not match");
    expectBadEnglishIndex("a 0 0\n", "strictly increasing");
    expectBadEnglishIndex("ab 0\n", "one Unicode code point");
    expectBadEnglishIndex("a 0\na 0\n", "duplicate index key");
    expectBadEnglishIndex("a 0\nz 0\n", "not found in dictionary");
    fixture.write("en_index.dat",
                  "a 0\ne 0 1\nh 1\nl 0 1\no 1\np 0\n");
    require(DictionaryRepository::load(sources(options))->wordCount(0) == 2,
            "valid repository failed after rejected partial loads");
    (void)englishIndex;
}

void testRealData()
{
    const ServerOptions options = Configuration::load(SEARCHENGINE_TEST_CONFIG);
    const std::shared_ptr<const DictionaryRepository> repository =
        options.keywordIndexRoot.empty()
            ? DictionaryRepository::load(sources(options))
            : DictionaryStore::loadCurrent(options.keywordIndexRoot).repository;
    require(repository->wordCount(0) > 1000 && repository->wordCount(1) > 1000,
            "real dictionaries did not load fully");
    require(!repository->findCandidates({"a", "中"}).empty(),
            "real indexes returned no candidates");
}

} // namespace

int main()
{
    try {
        testFixtures();
        testRealData();
    } catch (const std::exception &error) {
        std::cerr << "DictionaryRepositoryTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
