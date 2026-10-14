#include "corpus/ChineseKeywordProcessor.h"
#include "corpus/KeywordProcessor.h"
#include "DictionaryStore.h"
#include "DirectoryScanner.h"

#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct BuildOptions
{
    std::string englishCorpusDirectory;
    std::string chineseCorpusDirectory;
    std::string englishStopWords;
    std::string chineseStopWords;
    std::string englishDictionary;
    std::string englishIndex;
    std::string chineseDictionary;
    std::string chineseIndex;
    std::string jiebaDictionaryDirectory;
    std::string keywordIndexRoot;
};

BuildOptions defaultOptions()
{
    BuildOptions options;
    options.englishCorpusDirectory = "docs/模块一/yuliao/english";
    options.chineseCorpusDirectory = "docs/模块一/yuliao/chinese";
    options.englishStopWords =
        "docs/模块一/yuliao/stop_words/stop_words_eng.txt";
    options.chineseStopWords =
        "docs/模块一/yuliao/stop_words/stop_words_zh.txt";
    options.keywordIndexRoot = "data/keyword_index";
    return options;
}

BuildOptions parseOptions(int argc, char *argv[])
{
    BuildOptions options = defaultOptions();
    if (argc == 1) {
        return options;
    }
    if (argc == 3 && std::string(argv[1]) == "--index-root") {
        options.keywordIndexRoot = argv[2];
        return options;
    }
    if (argc == 6 || argc == 7) {
        options.englishCorpusDirectory = argv[1];
        options.chineseCorpusDirectory = argv[2];
        options.englishStopWords = argv[3];
        options.chineseStopWords = argv[4];
        options.keywordIndexRoot = argv[5];
        if (argc == 7) options.jiebaDictionaryDirectory = argv[6];
        return options;
    }
    if (argc != 9 && argc != 10) {
        throw std::invalid_argument(
            "usage: " + std::string(argv[0]) +
            " [--index-root ROOT] or"
            " [english_corpus_directory chinese_corpus_directory"
            " stop_words_eng.txt stop_words_zh.txt index_root"
            " [jieba_dict_directory]] or legacy"
            " [english_corpus_directory chinese_corpus_directory"
            " stop_words_eng.txt stop_words_zh.txt en_dict.dat en_index.dat"
            " cn_dict.dat cn_index.dat [jieba_dict_directory]]");
    }

    options.englishCorpusDirectory = argv[1];
    options.chineseCorpusDirectory = argv[2];
    options.englishStopWords = argv[3];
    options.chineseStopWords = argv[4];
    options.englishDictionary = argv[5];
    options.englishIndex = argv[6];
    options.chineseDictionary = argv[7];
    options.chineseIndex = argv[8];
    options.keywordIndexRoot.clear();
    if (argc == 10) {
        options.jiebaDictionaryDirectory = argv[9];
    }
    return options;
}

bool hasTextExtension(const std::string &filename)
{
    return filename.size() >= 4 &&
           filename.compare(filename.size() - 4, 4, ".txt") == 0;
}

std::vector<std::string> scanTextFiles(const std::string &directory,
                                       const std::string &language)
{
    std::vector<std::string> files;
    for (const std::string &filename : DirectoryScanner::scan(directory)) {
        if (hasTextExtension(filename)) {
            files.push_back(filename);
        }
    }
    if (files.empty()) {
        throw std::runtime_error(
            "no .txt " + language + " corpus files in " + directory);
    }
    return files;
}

void validateOutputPaths(const BuildOptions &options)
{
    if (!options.keywordIndexRoot.empty()) return;
    const std::vector<std::string> outputs = {
        options.englishDictionary,
        options.englishIndex,
        options.chineseDictionary,
        options.chineseIndex
    };
    std::set<std::string> unique;
    for (const std::string &output : outputs) {
        if (output.empty()) {
            throw std::invalid_argument("corpus output path must not be empty");
        }
        if (!unique.insert(output).second) {
            throw std::invalid_argument(
                "all four corpus output paths must be distinct: " + output);
        }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    try {
        const BuildOptions options = parseOptions(argc, argv);
        validateOutputPaths(options);
        const std::vector<std::string> englishFiles = scanTextFiles(
            options.englishCorpusDirectory, "English");
        const std::vector<std::string> chineseFiles = scanTextFiles(
            options.chineseCorpusDirectory, "Chinese");

        KeywordProcessor english;
        ChineseKeywordProcessor chinese(options.jiebaDictionaryDirectory);

        // 两种语言都处理成功后才开始写文件，避免处理阶段失败造成部分更新。
        english.processEnglish(englishFiles, options.englishStopWords);
        chinese.process(chineseFiles, options.chineseStopWords);

        std::string generation;
        if (!options.keywordIndexRoot.empty()) {
            std::vector<std::string> inputs = englishFiles;
            inputs.insert(inputs.end(), chineseFiles.begin(), chineseFiles.end());
            inputs.push_back(options.englishStopWords);
            inputs.push_back(options.chineseStopWords);
            const std::string directory = options.jiebaDictionaryDirectory.empty()
                ? ChineseTokenizer::defaultDictionaryDirectory()
                : options.jiebaDictionaryDirectory;
            for (const char *name : {"jieba.dict.utf8", "hmm_model.utf8",
                                     "user.dict.utf8", "idf.utf8",
                                     "stop_words.utf8"}) {
                inputs.push_back(directory + (directory.back() == '/' ? "" : "/") +
                                 name);
            }
            generation = DictionaryStore::publish(
                options.keywordIndexRoot,
                [&](const DictionaryFiles &files) {
                    english.storeEnglish(files.englishDictionary, files.englishIndex);
                    chinese.store(files.chineseDictionary, files.chineseIndex);
                }, inputs).name;
        } else {
            english.storeEnglish(options.englishDictionary, options.englishIndex);
            chinese.store(options.chineseDictionary, options.chineseIndex);
        }

        std::cout << "English corpus files: " << englishFiles.size() << '\n'
                  << "English dictionary entries: "
                  << english.englishDictionary().size() << '\n'
                  << "English index keys: " << english.englishIndex().size() << '\n'
                  << "Chinese corpus files: " << chineseFiles.size() << '\n'
                  << "Chinese dictionary entries: "
                  << chinese.dictionary().size() << '\n'
                  << "Chinese index keys: " << chinese.index().size() << '\n';
        if (!generation.empty()) {
            std::cout << "Dictionary generation: " << generation << '\n';
        }
    } catch (const std::invalid_argument &error) {
        std::cerr << error.what() << '\n';
        return 2;
    } catch (const std::exception &error) {
        std::cerr << "failed to build corpus: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
