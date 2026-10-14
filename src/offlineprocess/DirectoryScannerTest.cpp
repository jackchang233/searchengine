#include "DirectoryScanner.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

void createFile(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        throw std::system_error(
            errno, std::generic_category(), "open: " + path);
    }
    ::close(fd);
}

} // namespace

int main()
{
    char pathTemplate[] = "/tmp/directory-scanner-test.XXXXXX";
    char *createdDirectory = ::mkdtemp(pathTemplate);
    if (createdDirectory == nullptr) {
        throw std::system_error(errno, std::generic_category(), "mkdtemp");
    }

    const std::string root(createdDirectory);
    const std::string nested = root + "/nested";
    assert(::mkdir(nested.c_str(), 0700) == 0);

    const std::string rootFile = root + "/b.txt";
    const std::string nestedFile = nested + "/a.txt";
    createFile(rootFile);
    createFile(nestedFile);

    // 两类符号链接都应忽略，并且目录链接不能造成递归循环。
    assert(::symlink(rootFile.c_str(), (root + "/file-link").c_str()) == 0);
    assert(::symlink(root.c_str(), (nested + "/directory-loop").c_str()) == 0);

    const std::vector<std::string> files = DirectoryScanner::scan(root);
    assert(files.size() == 2);
    assert(files[0] == rootFile);
    assert(files[1] == nestedFile);

    bool invalidPathRejected = false;
    try {
        DirectoryScanner::scan(root + "/missing");
    } catch (const std::system_error &) {
        invalidPathRejected = true;
    }
    assert(invalidPathRejected);

    assert(::unlink((root + "/file-link").c_str()) == 0);
    assert(::unlink((nested + "/directory-loop").c_str()) == 0);
    assert(::unlink(nestedFile.c_str()) == 0);
    assert(::unlink(rootFile.c_str()) == 0);
    assert(::rmdir(nested.c_str()) == 0);
    assert(::rmdir(root.c_str()) == 0);
    return 0;
}
