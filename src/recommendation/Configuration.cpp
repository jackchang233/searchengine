#include "Configuration.h"

#include <arpa/inet.h>
#include <stdlib.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace {

std::string trim(const std::string &value)
{
    const std::string whitespace = " \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::string canonicalFile(const std::string &path)
{
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) {
        throw std::runtime_error("cannot resolve path " + path + ": " +
                                 std::strerror(errno));
    }
    const std::string result(resolved);
    std::free(resolved);
    return result;
}

std::string configValue(const std::map<std::string, std::string> &values,
                        const std::string &key)
{
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) {
        throw std::runtime_error("missing configuration key: " + key);
    }
    return found->second;
}

std::size_t positiveNumber(const std::map<std::string, std::string> &values,
                           const std::string &key, std::size_t fallback,
                           std::size_t maximum)
{
    const auto found = values.find(key);
    const std::string raw = found == values.end() ? std::to_string(fallback)
                                                   : found->second;
    if (raw.empty()) {
        throw std::runtime_error("invalid configuration value for " + key);
    }
    std::size_t result = 0;
    for (char digit : raw) {
        if (digit < '0' || digit > '9' ||
            result > (maximum - static_cast<std::size_t>(digit - '0')) / 10) {
            throw std::runtime_error("invalid configuration value for " + key +
                                     ": " + raw);
        }
        result = result * 10 + static_cast<std::size_t>(digit - '0');
    }
    if (result == 0) {
        throw std::runtime_error("configuration value must be positive: " + key);
    }
    return result;
}

std::string resolveDataPath(const std::map<std::string, std::string> &values,
                            const std::string &key, const std::string &directory)
{
    const std::string raw = configValue(values, key);
    return canonicalFile(raw[0] == '/' ? raw : directory + "/" + raw);
}

} // namespace

ServerOptions Configuration::load(
    const std::string &filename,
    const std::map<std::string, std::string> &overrides)
{
    const std::string absoluteConfig = canonicalFile(filename);
    std::ifstream input(absoluteConfig.c_str(), std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open configuration: " + absoluteConfig);
    }

    std::map<std::string, std::string> values;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const std::string entry = trim(line);
        if (entry.empty() || entry[0] == '#') {
            continue;
        }
        const std::size_t equal = entry.find('=');
        if (equal == std::string::npos) {
            throw std::runtime_error(absoluteConfig + ":" +
                std::to_string(lineNumber) + ": expected key = value");
        }
        const std::string key = trim(entry.substr(0, equal));
        const std::string value = trim(entry.substr(equal + 1));
        if (key.empty() || value.empty() || key.find_first_of(" \t") !=
            std::string::npos) {
            throw std::runtime_error(absoluteConfig + ":" +
                std::to_string(lineNumber) + ": invalid key or empty value");
        }
        if (!values.emplace(key, value).second) {
            throw std::runtime_error(absoluteConfig + ":" +
                std::to_string(lineNumber) + ": duplicate key: " + key);
        }
    }
    if (input.bad()) {
        throw std::runtime_error("failed to read configuration: " + absoluteConfig);
    }
    const std::set<std::string> overrideKeys{
        "en_dict_path", "en_dict_index_path", "cn_dict_path",
        "cn_dict_index_path", "server_ip", "server_port", "thread_num",
        "buf_size", "default_top_k", "max_top_k", "max_query_chars",
        "max_body_bytes", "max_json_depth", "max_inflight_per_connection",
        "shutdown_drain_timeout_ms", "web_index_root", "jieba_dict_dir",
        "stop_words_en", "stop_words_zh", "keyword_index_root"};
    for (const auto &entry : overrides) {
        if (overrideKeys.count(entry.first) == 0 || trim(entry.second).empty()) {
            throw std::runtime_error("invalid configuration override: " + entry.first);
        }
        values[entry.first] = trim(entry.second);
    }

    const std::string directory =
        absoluteConfig.substr(0, absoluteConfig.find_last_of('/'));
    ServerOptions options;
    if (values.count("keyword_index_root") != 0) {
        options.keywordIndexRoot =
            resolveDataPath(values, "keyword_index_root", directory);
    } else {
        options.enDictionaryPath = resolveDataPath(values, "en_dict_path", directory);
        options.enIndexPath = resolveDataPath(values, "en_dict_index_path", directory);
        options.cnDictionaryPath = resolveDataPath(values, "cn_dict_path", directory);
        options.cnIndexPath = resolveDataPath(values, "cn_dict_index_path", directory);
    }
    if (values.count("web_index_root") != 0) {
        options.webIndexRoot = resolveDataPath(values, "web_index_root", directory);
        options.jiebaDictionaryDirectory =
            resolveDataPath(values, "jieba_dict_dir", directory);
        options.webStopWordFiles.push_back(
            resolveDataPath(values, "stop_words_zh", directory));
        options.webStopWordFiles.push_back(
            resolveDataPath(values, "stop_words_en", directory));
    }
    if (options.keywordIndexRoot.empty()) {
        const std::set<std::string> paths{
            options.enDictionaryPath, options.enIndexPath,
            options.cnDictionaryPath, options.cnIndexPath};
        if (paths.size() != 4) {
            throw std::runtime_error("dictionary and index paths must be distinct");
        }
    }

    options.serverIp = configValue(values, "server_ip");
    struct in_addr address;
    if (::inet_pton(AF_INET, options.serverIp.c_str(), &address) != 1) {
        throw std::runtime_error("invalid configuration value for server_ip");
    }
    options.serverPort = static_cast<unsigned short>(
        positiveNumber(values, "server_port", 8888, 65535));
    options.threadCount = positiveNumber(values, "thread_num", 4, 65536);
    options.queueCapacity = positiveNumber(values, "buf_size", 1024,
                                           std::numeric_limits<std::size_t>::max());
    options.defaultTopK = positiveNumber(values, "default_top_k", 5, 20);
    options.maxTopK = positiveNumber(values, "max_top_k", 20, 20);
    if (options.defaultTopK > options.maxTopK) {
        throw std::runtime_error("default_top_k exceeds max_top_k");
    }
    options.maxQueryChars = positiveNumber(values, "max_query_chars", 64, 4096);
    options.maxBodyBytes = positiveNumber(values, "max_body_bytes", 4096,
                                          16 * 1024 * 1024);
    options.maxJsonDepth = positiveNumber(values, "max_json_depth", 8, 1024);
    options.maxInflightPerConnection = positiveNumber(
        values, "max_inflight_per_connection", 16, 65536);
    options.shutdownDrainTimeoutMs = positiveNumber(
        values, "shutdown_drain_timeout_ms", 5000, 24 * 60 * 60 * 1000);
    return options;
}
