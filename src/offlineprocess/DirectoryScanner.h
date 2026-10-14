#pragma once

#include <string>
#include <vector>

/**
 * 递归扫描目录中的普通文件。
 *
 * 返回值使用绝对路径，并按字典序排序，以保证离线建库结果可重复。
 * 符号链接和其他特殊文件会被忽略，目录读取失败时抛出 std::system_error。
 */
class DirectoryScanner
{
public:
    static std::vector<std::string> scan(const std::string &dir);

private:
    DirectoryScanner() = delete;
};
