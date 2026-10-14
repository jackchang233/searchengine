#pragma once

// 语料处理模块：cppjieba 中文分词封装。

#include <memory>
#include <string>
#include <vector>

namespace cppjieba {
class Jieba;
}

/** 复用一个 cppjieba 实例，以 Mix 模式进行中文分词。 */
class ChineseTokenizer
{
public:
    /** 空路径使用 CMake 检测到的词典目录；非空路径须包含五个资源文件。 */
    explicit ChineseTokenizer(
        const std::string &dictionaryDirectory = std::string());
    ~ChineseTokenizer();

    ChineseTokenizer(const ChineseTokenizer &) = delete;
    ChineseTokenizer &operator=(const ChineseTokenizer &) = delete;

    void cut(const std::string &sentence,
             std::vector<std::string> &words) const;

    static std::string defaultDictionaryDirectory();

private:
    std::unique_ptr<cppjieba::Jieba> _jieba;
};
