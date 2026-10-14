#include "DictionaryStore.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

const char kEnglishDictionary[] = "en_dict.dat";
const char kEnglishIndex[] = "en_index.dat";
const char kChineseDictionary[] = "cn_dict.dat";
const char kChineseIndex[] = "cn_index.dat";
const char kManifest[] = "manifest.json";

std::string join(const std::string &directory, const std::string &name)
{
    return directory + (directory.back() == '/' ? "" : "/") + name;
}

void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}

class Descriptor
{
public:
    explicit Descriptor(int fd) : _fd(fd) {}
    ~Descriptor() { if (_fd >= 0) ::close(_fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    Descriptor(Descriptor &&other) noexcept : _fd(other._fd) { other._fd = -1; }
    int get() const { return _fd; }
    void close()
    {
        if (_fd < 0) return;
        const int fd = _fd;
        _fd = -1;
        if (::close(fd) != 0) {
            throw std::system_error(errno, std::generic_category(), "close file");
        }
    }
private:
    int _fd;
};

Descriptor openFile(const std::string &path, int flags)
{
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }
    return Descriptor(fd);
}

void makeDirectory(const std::string &path)
{
    if (::mkdir(path.c_str(), 0755) == 0) return;
    if (errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "mkdir " + path);
    }
    struct stat status;
    require(::stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode),
            "not a directory: " + path);
}

std::string absoluteDirectory(const std::string &path)
{
    char *resolved = ::realpath(path.c_str(), nullptr);
    if (!resolved) {
        throw std::system_error(errno, std::generic_category(), "realpath " + path);
    }
    const std::string result(resolved);
    ::free(resolved);
    return result;
}

void syncFile(const std::string &path)
{
    Descriptor file = openFile(path, O_RDONLY);
    if (::fsync(file.get()) != 0) {
        throw std::system_error(errno, std::generic_category(), "fsync " + path);
    }
}

void syncDirectory(const std::string &path)
{
    Descriptor directory = openFile(path, O_RDONLY | O_DIRECTORY);
    if (::fsync(directory.get()) != 0) {
        throw std::system_error(errno, std::generic_category(), "fsync " + path);
    }
}

struct Digest
{
    std::uint64_t size;
    std::string sha256;
};

Digest digestFile(const std::string &path)
{
    Descriptor file = openFile(path, O_RDONLY);
    struct stat status;
    require(::fstat(file.get(), &status) == 0 && S_ISREG(status.st_mode),
            "not a regular file: " + path);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
            "cannot initialize SHA-256");
    char buffer[64 * 1024];
    std::uint64_t size = 0;
    while (true) {
        const ssize_t count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "read " + path);
        }
        if (count == 0) break;
        require(size <= std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(count), "file size overflow");
        size += static_cast<std::uint64_t>(count);
        require(EVP_DigestUpdate(context.get(), buffer,
                                 static_cast<std::size_t>(count)) == 1,
                "SHA-256 update failed");
    }
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    require(EVP_DigestFinal_ex(context.get(), hash, &length) == 1 && length == 32,
            "SHA-256 finalization failed");
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < length; ++index) {
        hex << std::setw(2) << static_cast<unsigned int>(hash[index]);
    }
    return Digest{size, hex.str()};
}

bool validGeneration(const std::string &name)
{
    if (name.size() != 10 || name.compare(0, 4, "gen-") != 0) return false;
    for (std::size_t index = 4; index < name.size(); ++index) {
        const char character = name[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9'))) return false;
    }
    return true;
}

DictionaryFiles filesAt(const std::string &directory)
{
    return DictionaryFiles{join(directory, kEnglishDictionary),
                           join(directory, kEnglishIndex),
                           join(directory, kChineseDictionary),
                           join(directory, kChineseIndex)};
}

std::uint64_t unsignedField(const nlohmann::json &object, const char *key)
{
    require(object.contains(key) && object.at(key).is_number_unsigned(),
            std::string("invalid manifest field: ") + key);
    return object.at(key).get<std::uint64_t>();
}

void checkFile(const nlohmann::json &files, const std::string &directory,
               const char *name)
{
    require(files.contains(name) && files.at(name).is_object(),
            std::string("missing manifest file entry: ") + name);
    const nlohmann::json &entry = files.at(name);
    require(entry.contains("sha256") && entry.at("sha256").is_string(),
            std::string("missing SHA-256 for ") + name);
    const Digest actual = digestFile(join(directory, name));
    require(actual.size == unsignedField(entry, "size") &&
                actual.sha256 == entry.at("sha256").get<std::string>(),
            std::string("file size or checksum differs: ") + name);
}

DictionaryGeneration loadGeneration(const std::string &directory,
                                    const std::string &generation)
{
    require(validGeneration(generation), "invalid dictionary generation name");
    std::ifstream input(join(directory, kManifest).c_str(), std::ios::binary);
    require(static_cast<bool>(input), "cannot open dictionary manifest");
    const nlohmann::json manifest = nlohmann::json::parse(input);
    require(manifest.is_object() && unsignedField(manifest, "format_version") == 1 &&
                manifest.contains("generation") &&
                manifest.at("generation").is_string() &&
                manifest.at("generation").get<std::string>() == generation,
            "dictionary manifest generation or format does not match");
    require(manifest.contains("files") && manifest.at("files").is_object(),
            "dictionary manifest has no files");
    for (const char *name : {kEnglishDictionary, kEnglishIndex,
                             kChineseDictionary, kChineseIndex}) {
        checkFile(manifest.at("files"), directory, name);
    }
    require(manifest.contains("inputs") && manifest.at("inputs").is_array(),
            "dictionary manifest has no build inputs");
    for (const auto &entry : manifest.at("inputs")) {
        require(entry.is_object() && entry.contains("path") &&
                    entry.at("path").is_string() &&
                    entry.contains("sha256") && entry.at("sha256").is_string(),
                "invalid dictionary build input");
        (void)unsignedField(entry, "size");
    }
    const std::shared_ptr<const DictionaryRepository> repository =
        DictionaryRepository::load(filesAt(directory).sources());
    require(repository->wordCount(0) == unsignedField(manifest, "english_words") &&
                repository->wordCount(1) == unsignedField(manifest, "chinese_words"),
            "dictionary manifest word counts differ");
    return DictionaryGeneration{generation, repository};
}

class StagingDirectory
{
public:
    explicit StagingDirectory(const std::string &generations)
    {
        const std::string pattern = join(generations, ".staging.XXXXXX");
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        char *created = ::mkdtemp(name.data());
        if (!created) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp " + pattern);
        }
        _path = created;
    }
    ~StagingDirectory()
    {
        if (!_active) return;
        for (const char *name : {kEnglishDictionary, kEnglishIndex,
                                 kChineseDictionary, kChineseIndex, kManifest}) {
            ::unlink(join(_path, name).c_str());
        }
        ::rmdir(_path.c_str());
    }
    const std::string &path() const { return _path; }
    std::string generation() const
    {
        return "gen-" + _path.substr(_path.size() - 6);
    }
    void release() { _active = false; }
private:
    std::string _path;
    bool _active = true;
};

void writeAll(int fd, const std::string &content)
{
    std::size_t position = 0;
    while (position < content.size()) {
        const ssize_t count = ::write(fd, content.data() + position,
                                      content.size() - position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            throw std::system_error(errno, std::generic_category(), "write CURRENT");
        }
        position += static_cast<std::size_t>(count);
    }
}

void publishCurrent(const std::string &root, const std::string &generation)
{
    const std::string pattern = join(root, ".CURRENT.XXXXXX");
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "mkstemp CURRENT");
    }
    const std::string temporary = name.data();
    try {
        Descriptor output(fd);
        writeAll(output.get(), generation + "\n");
        if (::fsync(output.get()) != 0) {
            throw std::system_error(errno, std::generic_category(), "fsync CURRENT");
        }
        output.close();
        if (::rename(temporary.c_str(), join(root, "CURRENT").c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "publish CURRENT");
        }
        try { syncDirectory(root); }
        catch (const std::exception &error) {
            throw std::runtime_error(
                "CURRENT switched but directory durability is uncertain: " +
                std::string(error.what()));
        }
    } catch (...) {
        ::unlink(temporary.c_str());
        throw;
    }
}

} // namespace

std::vector<DictionarySource> DictionaryFiles::sources() const
{
    return {{"en", englishDictionary, englishIndex},
            {"cn", chineseDictionary, chineseIndex}};
}

DictionaryGeneration DictionaryStore::publish(
    const std::string &root, const Writer &writeFiles,
    const std::vector<std::string> &buildInputs)
{
    if (root.empty() || !writeFiles) {
        throw std::invalid_argument("dictionary root and writer are required");
    }
    makeDirectory(root);
    const std::string absoluteRoot = absoluteDirectory(root);
    const std::string generations = join(absoluteRoot, "generations");
    makeDirectory(generations);
    StagingDirectory staging(generations);
    const DictionaryFiles paths = filesAt(staging.path());
    writeFiles(paths);
    nlohmann::json manifest;
    manifest["format_version"] = 1U;
    manifest["generation"] = staging.generation();
    const std::shared_ptr<const DictionaryRepository> repository =
        DictionaryRepository::load(paths.sources());
    manifest["english_words"] = repository->wordCount(0);
    manifest["chinese_words"] = repository->wordCount(1);
    manifest["files"] = nlohmann::json::object();
    for (const char *name : {kEnglishDictionary, kEnglishIndex,
                             kChineseDictionary, kChineseIndex}) {
        const std::string path = join(staging.path(), name);
        syncFile(path);
        const Digest digest = digestFile(path);
        manifest["files"][name] =
            {{"size", digest.size}, {"sha256", digest.sha256}};
    }
    manifest["inputs"] = nlohmann::json::array();
    for (const std::string &path : buildInputs) {
        const Digest digest = digestFile(path);
        manifest["inputs"].push_back(
            {{"path", path}, {"size", digest.size}, {"sha256", digest.sha256}});
    }
    const std::string manifestPath = join(staging.path(), kManifest);
    {
        std::ofstream output(manifestPath.c_str(), std::ios::binary);
        require(static_cast<bool>(output), "cannot create dictionary manifest");
        output << manifest.dump(2) << '\n';
        output.close();
        require(static_cast<bool>(output), "cannot write dictionary manifest");
    }
    syncFile(manifestPath);
    DictionaryGeneration result = loadGeneration(staging.path(), staging.generation());
    syncDirectory(staging.path());

    Descriptor lock = openFile(join(absoluteRoot, "PUBLISH.lock"), O_RDWR | O_CREAT);
    if (::flock(lock.get(), LOCK_EX) != 0) {
        throw std::system_error(errno, std::generic_category(), "lock publisher");
    }
    const std::string finalDirectory = join(generations, staging.generation());
    struct stat existing;
    require(::lstat(finalDirectory.c_str(), &existing) != 0 && errno == ENOENT,
            "dictionary generation name already exists");
    if (::rename(staging.path().c_str(), finalDirectory.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "publish dictionary generation");
    }
    staging.release();
    syncDirectory(generations);
    publishCurrent(absoluteRoot, result.name);
    return result;
}

DictionaryGeneration DictionaryStore::loadCurrent(const std::string &root)
{
    if (root.empty()) {
        throw std::invalid_argument("dictionary root must not be empty");
    }
    const std::string absoluteRoot = absoluteDirectory(root);
    std::ifstream input(join(absoluteRoot, "CURRENT").c_str(), std::ios::binary);
    require(static_cast<bool>(input), "cannot open CURRENT dictionary generation");
    std::string generation, extra;
    require(static_cast<bool>(std::getline(input, generation)) &&
                validGeneration(generation) && !std::getline(input, extra),
            "invalid CURRENT dictionary generation");
    return loadGeneration(join(join(absoluteRoot, "generations"), generation),
                          generation);
}

DictionaryGeneration DictionaryStore::activate(
    const std::string &root, const std::string &generation)
{
    if (root.empty() || !validGeneration(generation)) {
        throw std::invalid_argument("dictionary root or generation is invalid");
    }
    const std::string absoluteRoot = absoluteDirectory(root);
    Descriptor lock = openFile(join(absoluteRoot, "PUBLISH.lock"), O_RDWR | O_CREAT);
    if (::flock(lock.get(), LOCK_EX) != 0) {
        throw std::system_error(errno, std::generic_category(), "lock publisher");
    }
    DictionaryGeneration selected = loadGeneration(
        join(join(absoluteRoot, "generations"), generation), generation);
    publishCurrent(absoluteRoot, generation);
    return selected;
}
