#include "corpus/KeywordProcessor.h"

#include <exception>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[])
{
    std::vector<std::string> corpusFiles;
    std::string stopWordsFile;
    std::string dictionaryFile;
    std::string indexFile;

    if (argc == 1) {
        corpusFiles.push_back("docs/模块一/yuliao/english/english.txt");
        corpusFiles.push_back(
            "docs/模块一/yuliao/english/The_Holy_Bible.txt");
        stopWordsFile =
            "docs/模块一/yuliao/stop_words/stop_words_eng.txt";
        dictionaryFile = "data/en_dict.dat";
        indexFile = "data/en_index.dat";
    } else if (argc == 6) {
        corpusFiles.push_back(argv[1]);
        corpusFiles.push_back(argv[2]);
        stopWordsFile = argv[3];
        dictionaryFile = argv[4];
        indexFile = argv[5];
    } else {
        std::cerr << "usage: " << argv[0]
                  << " [english.txt The_Holy_Bible.txt stop_words_eng.txt"
                  << " dictionary.dat index.dat]\n";
        return 2;
    }

    try {
        KeywordProcessor processor;
        processor.processEnglish(corpusFiles, stopWordsFile);
        processor.storeEnglish(dictionaryFile, indexFile);
        std::cout << "English dictionary entries: "
                  << processor.englishDictionary().size() << '\n'
                  << "English index keys: "
                  << processor.englishIndex().size() << '\n';
    } catch (const std::exception &error) {
        std::cerr << "failed to build English keywords: "
                  << error.what() << '\n';
        return 1;
    }
    return 0;
}
