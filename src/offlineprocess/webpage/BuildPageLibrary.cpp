#include "DirectoryScanner.h"
#include "webpage/PageProcessor.h"
#include "webpage/WebIndexStore.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool hasXmlExtension(const std::string &filename)
{
    return filename.size() >= 4 &&
           filename.compare(filename.size() - 4, 4, ".xml") == 0;
}

} // namespace

int main(int argc, char *argv[])
{
    std::string corpusDirectory = "docs/模块二/人民网语料";
    std::string indexRoot = "data/web_index";
    std::string chineseStopWords =
        "docs/模块一/yuliao/stop_words/stop_words_zh.txt";
    std::string englishStopWords =
        "docs/模块一/yuliao/stop_words/stop_words_eng.txt";
    std::string jiebaDictionaryDirectory;

    if (argc == 5 || argc == 6) {
        corpusDirectory = argv[1];
        indexRoot = argv[2];
        chineseStopWords = argv[3];
        englishStopWords = argv[4];
        if (argc == 6) {
            jiebaDictionaryDirectory = argv[5];
        }
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " [xml_corpus_directory index_root"
                  << " chinese_stop_words english_stop_words"
                  << " [jieba_dict_directory]]\n";
        return 2;
    }

    try {
        std::vector<std::string> xmlFiles;
        for (const std::string &filename : DirectoryScanner::scan(corpusDirectory)) {
            if (hasXmlExtension(filename)) {
                xmlFiles.push_back(filename);
            }
        }
        if (xmlFiles.empty()) {
            throw std::runtime_error("no .xml corpus files in " + corpusDirectory);
        }

        PageProcessor processor(jiebaDictionaryDirectory);
        processor.process(xmlFiles);
        processor.buildInvertedIndex({chineseStopWords, englishStopWords});
        WebIndexBuildInputs inputs;
        inputs.xmlFiles = xmlFiles;
        inputs.stopWordFiles = {chineseStopWords, englishStopWords};
        inputs.jiebaDictionaryDirectory = jiebaDictionaryDirectory;
        const WebIndexSnapshot published =
            WebIndexStore::publish(processor, indexRoot, inputs);
        const PageProcessor::Statistics &statistics = processor.statistics();
        std::size_t postingCount = 0;
        for (const auto &term : processor.invertedIndex()) {
            postingCount += term.second.size();
        }
        std::cout << "XML corpus files: " << xmlFiles.size() << '\n'
                  << "Extracted items: " << statistics.extracted << '\n'
                  << "Ignored items: " << statistics.ignored << '\n'
                  << "Duplicate items: " << statistics.duplicates << '\n'
                  << "Stored documents: " << processor.documents().size() << '\n'
                  << "Indexed terms: " << processor.invertedIndex().size() << '\n'
                  << "Index postings: " << postingCount << '\n'
                  << "Published generation: " << published.generation() << '\n';
    } catch (const std::exception &error) {
        std::cerr << "failed to build page data: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
