#include "corpus/ChineseKeywordProcessor.h"
#include "DirectoryScanner.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char *argv[])
{
    std::string corpusDirectory = "docs/模块一/yuliao/chinese";
    std::string stopWordsFile =
        "docs/模块一/yuliao/stop_words/stop_words_zh.txt";
    std::string dictionaryFile = "data/cn_dict.dat";
    std::string indexFile = "data/cn_index.dat";
    std::string jiebaDictionaryDirectory;

    if (argc == 5 || argc == 6) {
        corpusDirectory = argv[1];
        stopWordsFile = argv[2];
        dictionaryFile = argv[3];
        indexFile = argv[4];
        if (argc == 6) {
            jiebaDictionaryDirectory = argv[5];
        }
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [corpus_directory stop_words_zh.txt dictionary.dat"
                  << " index.dat [jieba_dict_directory]]\n";
        return 2;
    }

    try {
        std::vector<std::string> corpusFiles;
        for (const std::string &filename : DirectoryScanner::scan(corpusDirectory)) {
            if (filename.size() >= 4 &&
                filename.compare(filename.size() - 4, 4, ".txt") == 0) {
                corpusFiles.push_back(filename);
            }
        }
        if (corpusFiles.empty()) {
            throw std::runtime_error("no .txt corpus files in " + corpusDirectory);
        }

        ChineseKeywordProcessor processor(jiebaDictionaryDirectory);
        processor.process(corpusFiles, stopWordsFile);
        processor.store(dictionaryFile, indexFile);
        std::cout << "Chinese corpus files: " << corpusFiles.size() << '\n'
                  << "Chinese dictionary entries: "
                  << processor.dictionary().size() << '\n'
                  << "Chinese index keys: " << processor.index().size() << '\n';
    } catch (const std::exception &error) {
        std::cerr << "failed to build Chinese keywords: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
