#include "DirectoryScanner.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>

namespace {

using DirectoryPtr = std::unique_ptr<DIR, int (*)(DIR *)>;

std::string joinPath(const std::string &directory, const char *name)
{
    if (directory == "/") {
        return directory + name;
    }
    return directory + "/" + name;
}

void scanDirectory(const std::string &directory,
                   std::vector<std::string> &files)
{
    DIR *rawDirectory = ::opendir(directory.c_str());
    if (rawDirectory == nullptr) {
        throw std::system_error(
            errno, std::generic_category(), "opendir: " + directory);
    }
    DirectoryPtr directoryHandle(rawDirectory, &::closedir);

    while (true) {
        errno = 0;
        struct dirent *entry = ::readdir(directoryHandle.get());
        if (entry == nullptr) {
            if (errno != 0) {
                throw std::system_error(
                    errno, std::generic_category(),
                    "readdir: " + directory);
            }
            break;
        }

        const std::string name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }

        const std::string path = joinPath(directory, entry->d_name);
        struct stat status;
        if (::lstat(path.c_str(), &status) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "lstat: " + path);
        }

        if (S_ISDIR(status.st_mode)) {
            scanDirectory(path, files);
        } else if (S_ISREG(status.st_mode)) {
            files.push_back(path);
        }
    }
}

std::string canonicalDirectory(const std::string &directory)
{
    if (directory.empty()) {
        throw std::invalid_argument("directory path must not be empty");
    }

    char *resolved = ::realpath(directory.c_str(), nullptr);
    if (resolved == nullptr) {
        throw std::system_error(
            errno, std::generic_category(), "realpath: " + directory);
    }

    const std::string result(resolved);
    std::free(resolved);
    return result;
}

} // namespace

std::vector<std::string> DirectoryScanner::scan(const std::string &dir)
{
    std::vector<std::string> files;
    scanDirectory(canonicalDirectory(dir), files);
    std::sort(files.begin(), files.end());
    return files;
}
