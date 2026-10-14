#include "corpus/ChineseTokenizer.h"
#include "Utf8Utils.h"

#include <cppjieba/Jieba.hpp>

#include <fstream>
#include <stdexcept>

namespace {

std::string resourcePath(const std::string &directory,
                         const std::string &name)
{
    return directory + (directory.back() == '/' ? "" : "/") + name;
}

} // namespace

ChineseTokenizer::ChineseTokenizer(const std::string &dictionaryDirectory)
{
    const std::string directory = dictionaryDirectory.empty()
        ? defaultDictionaryDirectory() : dictionaryDirectory;
    const std::string dictionary = resourcePath(directory, "jieba.dict.utf8");
    const std::string model = resourcePath(directory, "hmm_model.utf8");
    const std::string userDictionary = resourcePath(directory, "user.dict.utf8");
    const std::string idf = resourcePath(directory, "idf.utf8");
    const std::string stopWords = resourcePath(directory, "stop_words.utf8");

    // 先检查路径，让缺失资源产生可捕获的异常，而非 cppjieba 的 fatal 日志。
    for (const std::string &filename :
         {dictionary, model, userDictionary, idf, stopWords}) {
        std::ifstream input(filename.c_str(), std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open cppjieba resource: " + filename);
        }
    }

    _jieba.reset(new cppjieba::Jieba(
        dictionary, model, userDictionary, idf, stopWords));
}

ChineseTokenizer::~ChineseTokenizer() = default;

void ChineseTokenizer::cut(const std::string &sentence,
                           std::vector<std::string> &words) const
{
    if (!Utf8Utils::isValid(sentence)) {
        throw std::invalid_argument("cannot segment invalid UTF-8 text");
    }
    _jieba->Cut(sentence, words, true);
}

std::string ChineseTokenizer::defaultDictionaryDirectory()
{
    return SEARCHENGINE_JIEBA_DICT_DIR;
}
