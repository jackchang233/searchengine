#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct ServerOptions
{
    std::string keywordIndexRoot;
    std::string enDictionaryPath;
    std::string enIndexPath;
    std::string cnDictionaryPath;
    std::string cnIndexPath;
    std::string webIndexRoot;
    std::string jiebaDictionaryDirectory;
    std::vector<std::string> webStopWordFiles;
    std::string serverIp;
    unsigned short serverPort;
    std::size_t threadCount;
    std::size_t queueCapacity;
    std::size_t defaultTopK;
    std::size_t maxTopK;
    std::size_t maxQueryChars;
    std::size_t maxBodyBytes;
    std::size_t maxJsonDepth;
    std::size_t maxInflightPerConnection;
    std::size_t shutdownDrainTimeoutMs;
};

class Configuration
{
public:
    // 相对文件路径以配置文件所在目录为基准；覆盖值也遵循相同规则。
    // 只读取当前在线服务使用的键，其他已有配置键保留供后续模块使用。
    static ServerOptions load(
        const std::string &filename,
        const std::map<std::string, std::string> &overrides = {});
};
