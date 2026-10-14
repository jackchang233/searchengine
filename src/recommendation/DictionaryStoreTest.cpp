#include "Configuration.h"
#include "DictionaryStore.h"

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Action>
void expectFailure(Action action)
{
    try { action(); }
    catch (const std::exception &) { return; }
    throw std::runtime_error("expected dictionary store failure");
}

void writeFile(const std::string &path, const std::string &contents)
{
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "cannot create fixture file");
    output << contents;
    output.close();
    require(static_cast<bool>(output), "cannot write fixture file");
}

std::string readFile(const std::string &path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    require(static_cast<bool>(input), "cannot open fixture file");
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        char pattern[] = "/tmp/dictionary-store-test.XXXXXX";
        char *created = ::mkdtemp(pattern);
        if (!created) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        _root = created;
    }

    ~TemporaryDirectory()
    {
        const std::string generations = indexRoot() + "/generations";
        DIR *directory = ::opendir(generations.c_str());
        if (directory) {
            while (dirent *entry = ::readdir(directory)) {
                const std::string name(entry->d_name);
                if (name == "." || name == "..") continue;
                const std::string path = generations + "/" + name;
                for (const char *file : {"en_dict.dat", "en_index.dat",
                                         "cn_dict.dat", "cn_index.dat",
                                         "manifest.json"}) {
                    ::unlink((path + "/" + file).c_str());
                }
                ::rmdir(path.c_str());
            }
            ::closedir(directory);
        }
        ::unlink((indexRoot() + "/CURRENT").c_str());
        ::unlink((indexRoot() + "/PUBLISH.lock").c_str());
        ::rmdir(generations.c_str());
        ::rmdir(indexRoot().c_str());
        ::unlink((_root + "/input.txt").c_str());
        ::unlink((_root + "/server.conf").c_str());
        ::rmdir(_root.c_str());
    }

    const std::string &root() const { return _root; }
    std::string indexRoot() const { return _root + "/index"; }

private:
    std::string _root;
};

DictionaryStore::Writer writer(const std::string &english,
                               const std::string &englishIndex,
                               const std::string &chinese,
                               const std::string &chineseIndex)
{
    return [=](const DictionaryFiles &files) {
        writeFile(files.englishDictionary, english);
        writeFile(files.englishIndex, englishIndex);
        writeFile(files.chineseDictionary, chinese);
        writeFile(files.chineseIndex, chineseIndex);
    };
}

void testPublishAndFailureIsolation()
{
    TemporaryDirectory temporary;
    const std::string input = temporary.root() + "/input.txt";
    writeFile(input, "fixture corpus\n");
    const DictionaryGeneration first = DictionaryStore::publish(
        temporary.indexRoot(), writer("apple 2\n", "a 0\ne 0\nl 0\np 0\n",
                                      "中国 5\n", "中 0\n国 0\n"), {input});
    require(first.repository->word(WordId{0, 0}).word == "apple" &&
                DictionaryStore::loadCurrent(temporary.indexRoot()).name == first.name,
            "first dictionary generation did not load");

    const std::string config = temporary.root() + "/server.conf";
    writeFile(config, "keyword_index_root = index\n"
                      "server_ip = 127.0.0.1\nserver_port = 8888\n"
                      "thread_num = 2\nbuf_size = 4\n");
    const ServerOptions options = Configuration::load(config);
    require(options.keywordIndexRoot == temporary.indexRoot() &&
                options.enDictionaryPath.empty(),
            "versioned dictionary config still requires legacy files");

    const DictionaryGeneration second = DictionaryStore::publish(
        temporary.indexRoot(), writer("berry 7\n", "b 0\ne 0\nr 0\ny 0\n",
                                      "中文 4\n", "中 0\n文 0\n"), {input});
    require(second.name != first.name &&
                DictionaryStore::loadCurrent(temporary.indexRoot())
                        .repository->word(WordId{0, 0}).word == "berry" &&
                first.repository->word(WordId{0, 0}).word == "apple",
            "version switch or existing snapshot failed");
    const std::string currentPath = temporary.indexRoot() + "/CURRENT";
    const std::string stableCurrent = readFile(currentPath);

    expectFailure([&] {
        DictionaryStore::publish(temporary.indexRoot(),
            [](const DictionaryFiles &files) {
                writeFile(files.englishDictionary, "partial 1\n");
                throw std::runtime_error("injected writer failure");
            }, {input});
    });
    expectFailure([&] {
        DictionaryStore::publish(
            temporary.indexRoot(),
            writer("apple 2\n", "a 1\n", "中国 5\n", "中 0\n国 0\n"),
            {input});
    });
    require(readFile(currentPath) == stableCurrent &&
                DictionaryStore::loadCurrent(temporary.indexRoot()).name == second.name,
            "failed publish changed CURRENT");

    expectFailure([&] {
        DictionaryStore::activate(temporary.indexRoot(), "gen-ABC123");
    });
    require(readFile(currentPath) == stableCurrent,
            "invalid rollback changed CURRENT");
    const DictionaryGeneration rolledBack =
        DictionaryStore::activate(temporary.indexRoot(), first.name);
    require(rolledBack.name == first.name &&
                DictionaryStore::loadCurrent(temporary.indexRoot())
                        .repository->word(WordId{0, 0}).word == "apple",
            "rollback did not select the previous generation");
    DictionaryStore::activate(temporary.indexRoot(), second.name);

    const std::string currentDirectory = temporary.indexRoot() +
        "/generations/" + second.name;
    writeFile(currentDirectory + "/en_dict.dat", "tampered 1\n");
    expectFailure([&] { DictionaryStore::loadCurrent(temporary.indexRoot()); });
    writeFile(currentDirectory + "/en_dict.dat", "berry 7\n");
    require(DictionaryStore::loadCurrent(temporary.indexRoot()).name == second.name,
            "restored generation did not load");
    ::unlink((currentDirectory + "/en_index.dat").c_str());
    expectFailure([&] { DictionaryStore::loadCurrent(temporary.indexRoot()); });
    writeFile(currentDirectory + "/en_index.dat", "b 0\ne 0\nr 0\ny 0\n");

    writeFile(currentPath, "../outside\n");
    expectFailure([&] { DictionaryStore::loadCurrent(temporary.indexRoot()); });
    writeFile(currentPath, stableCurrent);
    require(DictionaryStore::loadCurrent(temporary.indexRoot()).name == second.name,
            "restored CURRENT did not load");
}

} // namespace

int main()
{
    try { testPublishAndFailureIsolation(); }
    catch (const std::exception &error) {
        std::cerr << "DictionaryStoreTest failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
