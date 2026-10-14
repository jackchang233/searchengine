#include "webpage/WebIndexStore.h"
#include "Utf8Utils.h"

#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <tinyxml2.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
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

const char kPages[] = "webpage.dat";
const char kOffsets[] = "offset.dat";
const char kIndex[] = "inverted_index.dat";
const char kManifest[] = "manifest.json";

std::string pathJoin(const std::string &directory, const std::string &name)
{
    return directory + (directory.back() == '/' ? "" : "/") + name;
}

void require(bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FileDescriptor
{
public:
    explicit FileDescriptor(int fd) : _fd(fd) {}
    ~FileDescriptor() { if (_fd >= 0) ::close(_fd); }
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept : _fd(other._fd)
    {
        other._fd = -1;
    }
    int get() const { return _fd; }
    void close()
    {
        if (_fd >= 0) {
            const int fd = _fd;
            _fd = -1;
            if (::close(fd) != 0) {
                throw std::system_error(errno, std::generic_category(), "close file");
            }
        }
    }
private:
    int _fd;
};

FileDescriptor openFile(const std::string &path, int flags)
{
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open " + path);
    }
    return FileDescriptor(fd);
}

void makeDirectory(const std::string &path)
{
    if (::mkdir(path.c_str(), 0755) == 0) {
        return;
    }
    if (errno != EEXIST) {
        throw std::system_error(errno, std::generic_category(), "mkdir " + path);
    }
    struct stat status;
    if (::stat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
        throw std::runtime_error("not a directory: " + path);
    }
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
    FileDescriptor file = openFile(path, O_RDONLY);
    if (::fsync(file.get()) != 0) {
        throw std::system_error(errno, std::generic_category(), "fsync " + path);
    }
}

void syncDirectory(const std::string &path)
{
    FileDescriptor directory = openFile(path, O_RDONLY | O_DIRECTORY);
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
    FileDescriptor file = openFile(path, O_RDONLY);
    struct stat status;
    if (::fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw std::runtime_error("not a regular file: " + path);
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
            "cannot initialize SHA-256");

    char buffer[64 * 1024];
    std::uint64_t size = 0;
    while (true) {
        const ssize_t count = ::read(file.get(), buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw std::system_error(errno, std::generic_category(), "read " + path);
        }
        if (count == 0) {
            break;
        }
        require(size <= std::numeric_limits<std::uint64_t>::max() -
                    static_cast<std::uint64_t>(count), "file size overflow");
        size += static_cast<std::uint64_t>(count);
        require(EVP_DigestUpdate(context.get(), buffer,
                                 static_cast<std::size_t>(count)) == 1,
                "SHA-256 update failed");
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLength = 0;
    require(EVP_DigestFinal_ex(context.get(), hash, &hashLength) == 1 &&
                hashLength == 32, "SHA-256 finalization failed");
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < hashLength; ++index) {
        hex << std::setw(2) << static_cast<unsigned int>(hash[index]);
    }
    return Digest{size, hex.str()};
}

std::uint64_t decimal(const std::string &token, const std::string &field)
{
    require(!token.empty(), "missing " + field);
    std::uint64_t value = 0;
    for (char character : token) {
        require(character >= '0' && character <= '9', "invalid " + field);
        const unsigned int digit = static_cast<unsigned int>(character - '0');
        require(value <= (std::numeric_limits<std::uint64_t>::max() - digit) / 10,
                "overflow in " + field);
        value = value * 10 + digit;
    }
    return value;
}

double weightValue(const std::string &token)
{
    std::size_t end = 0;
    double value = 0;
    try {
        value = std::stod(token, &end);
    } catch (const std::exception &) {
        throw std::runtime_error("invalid posting weight");
    }
    require(end == token.size() && std::isfinite(value) && value > 0 &&
                value <= 1.0 + 1e-10, "invalid posting weight");
    return value;
}

std::string childText(const tinyxml2::XMLElement *root, const char *name)
{
    const tinyxml2::XMLElement *element = root->FirstChildElement(name);
    require(element != nullptr, std::string("missing doc field: ") + name);
    const char *text = element->GetText();
    return text ? text : "";
}

PageProcessor::Document parseDocument(const std::string &fragment,
                                      std::uint64_t expectedId)
{
    tinyxml2::XMLDocument parsed;
    require(parsed.Parse(fragment.c_str(), fragment.size()) == tinyxml2::XML_SUCCESS,
            "invalid XML in page library at document " + std::to_string(expectedId));
    const tinyxml2::XMLElement *root = parsed.FirstChildElement("doc");
    require(root != nullptr && root->NextSiblingElement() == nullptr,
            "invalid doc root at document " + std::to_string(expectedId));
    require(decimal(childText(root, "docid"), "docid") == expectedId,
            "page and offset document IDs differ");
    PageProcessor::Document document;
    document.id = expectedId;
    document.link = childText(root, "link");
    document.title = childText(root, "title");
    document.content = childText(root, "content");
    return document;
}

std::string readFragment(std::ifstream &pages,
                         const WebIndexSnapshot::Offset &offset)
{
    require(offset.begin <= static_cast<std::uint64_t>(
                std::numeric_limits<std::streamoff>::max()) &&
                offset.length <= static_cast<std::uint64_t>(
                std::numeric_limits<std::streamsize>::max()),
            "page offset exceeds stream limits");
    pages.clear();
    pages.seekg(static_cast<std::streamoff>(offset.begin));
    require(static_cast<bool>(pages), "cannot seek in page library");
    std::string fragment(static_cast<std::size_t>(offset.length), '\0');
    pages.read(&fragment[0], static_cast<std::streamsize>(offset.length));
    require(static_cast<bool>(pages), "cannot read page fragment");
    return fragment;
}

bool validGeneration(const std::string &name)
{
    if (name.size() < 5 || name.size() > 80 || name.compare(0, 4, "gen-") != 0) {
        return false;
    }
    for (std::size_t index = 4; index < name.size(); ++index) {
        const char character = name[index];
        if (!((character >= 'a' && character <= 'z') ||
              (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-')) {
            return false;
        }
    }
    return true;
}

std::uint64_t jsonUnsigned(const nlohmann::json &object, const char *key)
{
    require(object.contains(key) && object.at(key).is_number_unsigned(),
            std::string("invalid manifest field: ") + key);
    return object.at(key).get<std::uint64_t>();
}

void validateDigest(const nlohmann::json &files, const std::string &directory,
                    const char *name)
{
    require(files.contains(name) && files.at(name).is_object(),
            std::string("missing manifest file entry: ") + name);
    const nlohmann::json &entry = files.at(name);
    require(entry.contains("sha256") && entry.at("sha256").is_string(),
            std::string("missing SHA-256 for ") + name);
    const Digest actual = digestFile(pathJoin(directory, name));
    require(actual.size == jsonUnsigned(entry, "size") &&
                actual.sha256 == entry.at("sha256").get<std::string>(),
            std::string("file size or checksum differs: ") + name);
}

std::vector<WebIndexSnapshot::Offset> loadOffsets(
    const std::string &filename, const std::string &pageFile,
    std::uint64_t expectedDocuments, std::uint64_t pageSize)
{
    require(expectedDocuments < std::numeric_limits<std::size_t>::max(),
            "document count exceeds address space");
    std::ifstream input(filename.c_str(), std::ios::binary);
    std::ifstream pages(pageFile.c_str(), std::ios::binary);
    require(input && pages, "cannot open page or offset library");
    std::vector<WebIndexSnapshot::Offset> offsets;
    offsets.reserve(static_cast<std::size_t>(expectedDocuments));
    std::uint64_t nextBegin = 0;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string idToken, beginToken, lengthToken, extra;
        require((fields >> idToken >> beginToken >> lengthToken) && !(fields >> extra),
                "invalid offset line");
        const std::uint64_t id = decimal(idToken, "offset document ID");
        const std::uint64_t begin = decimal(beginToken, "offset begin");
        const std::uint64_t length = decimal(lengthToken, "offset length");
        require(offsets.size() < expectedDocuments && id == offsets.size() + 1 &&
                    begin == nextBegin && length > 0 && begin <= pageSize &&
                    length <= pageSize - begin, "page offsets are inconsistent");
        const WebIndexSnapshot::Offset offset{begin, length};
        parseDocument(readFragment(pages, offset), id);
        offsets.push_back(offset);
        nextBegin = begin + length;
    }
    require(input.eof() && offsets.size() == expectedDocuments &&
                nextBegin == pageSize, "offset library does not cover page library");
    return offsets;
}

PageProcessor::InvertedIndex loadIndex(
    const std::string &filename, std::uint64_t documentCount,
    std::uint64_t expectedTerms, std::uint64_t expectedPostings)
{
    require(documentCount < std::numeric_limits<std::size_t>::max(),
            "document count exceeds address space");
    std::ifstream input(filename.c_str(), std::ios::binary);
    require(static_cast<bool>(input), "cannot open inverted index");
    PageProcessor::InvertedIndex index;
    std::vector<long double> squaredNorms(static_cast<std::size_t>(documentCount + 1), 0);
    std::uint64_t postingCount = 0;
    std::string previousTerm;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string term;
        require(static_cast<bool>(fields >> term) && Utf8Utils::isValid(term) &&
                    (previousTerm.empty() || previousTerm < term),
                "invalid or unordered inverted-index term");
        std::vector<PageProcessor::Posting> postings;
        std::string idToken, weightToken;
        std::uint64_t lastId = 0;
        while (fields >> idToken) {
            require(static_cast<bool>(fields >> weightToken),
                    "posting has no weight");
            const std::uint64_t id = decimal(idToken, "posting document ID");
            const double weight = weightValue(weightToken);
            require(id > lastId && id <= documentCount,
                    "posting has invalid or unordered document ID");
            squaredNorms[static_cast<std::size_t>(id)] +=
                static_cast<long double>(weight) * weight;
            postings.emplace_back(id, weight);
            lastId = id;
            ++postingCount;
            require(postingCount <= expectedPostings, "too many index postings");
        }
        require(fields.eof() && !postings.empty(), "invalid inverted-index row");
        previousTerm = term;
        index.emplace(std::move(term), std::move(postings));
        require(index.size() <= expectedTerms, "too many index terms");
    }
    require(input.eof() && index.size() == expectedTerms &&
                postingCount == expectedPostings, "inverted-index counts differ");
    for (std::uint64_t id = 1; id <= documentCount; ++id) {
        const long double norm = squaredNorms[static_cast<std::size_t>(id)];
        require(norm == 0 || std::fabs(norm - 1.0L) <= 1e-6L,
                "document TF-IDF vector is not normalized");
    }
    return index;
}

nlohmann::json inputDigest(const std::string &path)
{
    const Digest digest = digestFile(path);
    return {{"path", path}, {"size", digest.size}, {"sha256", digest.sha256}};
}

nlohmann::json buildManifest(const PageProcessor &processor,
                             const std::string &directory,
                             const std::string &generation,
                             const WebIndexBuildInputs &inputs)
{
    nlohmann::json manifest;
    manifest["format_version"] = 1U;
    manifest["generation"] = generation;
    manifest["documents"] = processor.documents().size();
    manifest["terms"] = processor.invertedIndex().size();
    std::size_t postingCount = 0;
    for (const auto &term : processor.invertedIndex()) {
        postingCount += term.second.size();
    }
    manifest["postings"] = postingCount;
    manifest["files"] = nlohmann::json::object();
    for (const char *name : {kPages, kOffsets, kIndex}) {
        const Digest digest = digestFile(pathJoin(directory, name));
        manifest["files"][name] =
            {{"size", digest.size}, {"sha256", digest.sha256}};
    }
    manifest["build"] = {
        {"simhash_top_n", inputs.simhashTopN},
        {"hamming_threshold", inputs.hammingThreshold},
        {"tokenizer", "cppjieba Mix; Han and lowercase ASCII letters"}
    };
    manifest["inputs"] = nlohmann::json::object();
    for (const std::string &path : inputs.xmlFiles) {
        manifest["inputs"]["xml_files"].push_back(inputDigest(path));
    }
    for (const std::string &path : inputs.stopWordFiles) {
        manifest["inputs"]["stop_word_files"].push_back(inputDigest(path));
    }
    const std::string dict = inputs.jiebaDictionaryDirectory.empty()
        ? PageProcessor::defaultDictionaryDirectory()
        : inputs.jiebaDictionaryDirectory;
    for (const char *name : {"jieba.dict.utf8", "hmm_model.utf8",
                             "user.dict.utf8", "idf.utf8", "stop_words.utf8"}) {
        manifest["inputs"]["jieba_resources"].push_back(
            inputDigest(pathJoin(dict, name)));
    }
    return manifest;
}

class StagingDirectory
{
public:
    explicit StagingDirectory(const std::string &generations)
    {
        const std::string pattern = pathJoin(generations, ".staging.XXXXXX");
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
        if (!_active) {
            return;
        }
        for (const char *name : {kPages, kOffsets, kIndex, kManifest}) {
            ::unlink(pathJoin(_path, name).c_str());
        }
        ::rmdir(_path.c_str());
    }
    const std::string &path() const { return _path; }
    void release() { _active = false; }
    std::string generation() const
    {
        return "gen-" + _path.substr(_path.size() - 6);
    }
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
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::system_error(errno, std::generic_category(), "write CURRENT");
        }
        position += static_cast<std::size_t>(count);
    }
}

void publishCurrent(const std::string &root, const std::string &generation)
{
    const std::string pattern = pathJoin(root, ".CURRENT.XXXXXX");
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "mkstemp CURRENT");
    }
    const std::string temporary = name.data();
    try {
        FileDescriptor output(fd);
        writeAll(output.get(), generation + "\n");
        if (::fsync(output.get()) != 0) {
            throw std::system_error(errno, std::generic_category(), "fsync CURRENT");
        }
        output.close();
        if (::rename(temporary.c_str(), pathJoin(root, "CURRENT").c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(), "publish CURRENT");
        }
        try {
            syncDirectory(root);
        } catch (const std::exception &error) {
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

WebIndexSnapshot WebIndexStore::loadGeneration(
    const std::string &directory, const std::string &generation)
{
    require(validGeneration(generation), "invalid generation name");
    std::ifstream manifestFile(pathJoin(directory, kManifest).c_str());
    require(static_cast<bool>(manifestFile), "cannot open generation manifest");
    const nlohmann::json manifest = nlohmann::json::parse(manifestFile);
    require(manifest.is_object() && jsonUnsigned(manifest, "format_version") == 1 &&
                manifest.contains("generation") &&
                manifest.at("generation").is_string() &&
                manifest.at("generation").get<std::string>() == generation,
            "manifest generation or format does not match");
    require(manifest.contains("files") && manifest.at("files").is_object(),
            "manifest has no files");
    const nlohmann::json &files = manifest.at("files");
    validateDigest(files, directory, kPages);
    validateDigest(files, directory, kOffsets);
    validateDigest(files, directory, kIndex);

    const std::uint64_t documents = jsonUnsigned(manifest, "documents");
    const std::uint64_t terms = jsonUnsigned(manifest, "terms");
    const std::uint64_t postings = jsonUnsigned(manifest, "postings");
    WebIndexSnapshot snapshot;
    snapshot._generation = generation;
    require(manifest.contains("inputs") && manifest.at("inputs").is_object(),
            "manifest has no build inputs");
    const nlohmann::json &inputs = manifest.at("inputs");
    const auto loadFingerprints = [&](const char *name,
                                    std::vector<WebIndexSnapshot::InputFingerprint> &target) {
        require(inputs.contains(name) && inputs.at(name).is_array(),
                std::string("missing manifest input: ") + name);
        for (const auto &entry : inputs.at(name)) {
            require(entry.is_object() && entry.contains("sha256") &&
                        entry.at("sha256").is_string(),
                    std::string("invalid manifest input: ") + name);
            target.push_back({jsonUnsigned(entry, "size"),
                              entry.at("sha256").get<std::string>()});
        }
    };
    loadFingerprints("stop_word_files", snapshot._stopWordFingerprints);
    loadFingerprints("jieba_resources", snapshot._jiebaFingerprints);
    snapshot._pageFile = pathJoin(directory, kPages);
    snapshot._offsets = loadOffsets(pathJoin(directory, kOffsets),
                                    snapshot._pageFile, documents,
                                    jsonUnsigned(files.at(kPages), "size"));
    snapshot._index = loadIndex(pathJoin(directory, kIndex),
                                documents, terms, postings);
    std::shared_ptr<int> descriptor(new int(-1), [](int *owned) {
        if (*owned >= 0) {
            ::close(*owned);
        }
        delete owned;
    });
    const int fd = ::open(snapshot._pageFile.c_str(),
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "open " + snapshot._pageFile);
    }
    *descriptor = fd;
    snapshot._pageDescriptor = std::move(descriptor);
    return snapshot;
}

void WebIndexSnapshot::validateAnalyzerInputs(
    const std::string &dictionaryDirectory,
    const std::vector<std::string> &stopWordFiles) const
{
    require(stopWordFiles.size() == _stopWordFingerprints.size() &&
                _jiebaFingerprints.size() == 5,
            "online analyzer inputs differ from index generation");
    for (std::size_t index = 0; index < stopWordFiles.size(); ++index) {
        const Digest actual = digestFile(stopWordFiles[index]);
        require(actual.size == _stopWordFingerprints[index].size &&
                    actual.sha256 == _stopWordFingerprints[index].sha256,
                "online stop words differ from index generation");
    }
    const std::string directory = dictionaryDirectory.empty()
        ? PageProcessor::defaultDictionaryDirectory() : dictionaryDirectory;
    std::size_t index = 0;
    for (const char *name : {"jieba.dict.utf8", "hmm_model.utf8",
                             "user.dict.utf8", "idf.utf8", "stop_words.utf8"}) {
        const Digest actual = digestFile(pathJoin(directory, name));
        require(actual.size == _jiebaFingerprints[index].size &&
                    actual.sha256 == _jiebaFingerprints[index].sha256,
                "online cppjieba resources differ from index generation");
        ++index;
    }
}

PageProcessor::Document WebIndexSnapshot::readDocument(
    std::uint64_t documentId) const
{
    if (documentId == 0 || documentId > _offsets.size()) {
        throw std::out_of_range("document ID is outside current generation");
    }
    const Offset &offset = _offsets[documentId - 1];
    require(_pageDescriptor &&
                offset.begin <= static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()) &&
                offset.length <= std::numeric_limits<std::size_t>::max(),
            "page offset exceeds process limits");
    std::string fragment(static_cast<std::size_t>(offset.length), '\0');
    std::size_t position = 0;
    while (position < fragment.size()) {
        require(offset.begin + position <= static_cast<std::uint64_t>(
                    std::numeric_limits<off_t>::max()),
                "page offset exceeds pread limits");
        const ssize_t count = ::pread(*_pageDescriptor,
                                      &fragment[position], fragment.size() - position,
                                      static_cast<off_t>(offset.begin + position));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count > 0, "cannot read page fragment: " + _pageFile);
        position += static_cast<std::size_t>(count);
    }
    return parseDocument(fragment, documentId);
}

WebIndexSnapshot WebIndexStore::publish(
    const PageProcessor &processor, const std::string &root,
    const WebIndexBuildInputs &inputs)
{
    if (root.empty()) {
        throw std::invalid_argument("web index root must not be empty");
    }
    makeDirectory(root);
    const std::string absoluteRoot = absoluteDirectory(root);
    const std::string generations = pathJoin(absoluteRoot, "generations");
    makeDirectory(generations);
    StagingDirectory staging(generations);
    processor.store(pathJoin(staging.path(), kPages),
                    pathJoin(staging.path(), kOffsets));
    processor.storeInvertedIndex(pathJoin(staging.path(), kIndex));
    for (const char *name : {kPages, kOffsets, kIndex}) {
        syncFile(pathJoin(staging.path(), name));
    }
    const std::string manifestPath = pathJoin(staging.path(), kManifest);
    {
        std::ofstream output(manifestPath.c_str(), std::ios::binary);
        require(static_cast<bool>(output), "cannot create generation manifest");
        output << buildManifest(processor, staging.path(),
                                staging.generation(), inputs).dump(2) << '\n';
        output.close();
        require(static_cast<bool>(output), "cannot write generation manifest");
    }
    syncFile(manifestPath);
    WebIndexSnapshot snapshot = loadGeneration(staging.path(), staging.generation());
    syncDirectory(staging.path());

    const std::string finalDirectory =
        pathJoin(generations, staging.generation());
    // 锁只覆盖发布阶段；多个建库进程可以并行构建，但版本切换串行。
    FileDescriptor lock = openFile(pathJoin(absoluteRoot, "PUBLISH.lock"),
                                   O_RDWR | O_CREAT);
    if (::flock(lock.get(), LOCK_EX) != 0) {
        throw std::system_error(errno, std::generic_category(), "lock publisher");
    }
    struct stat existing;
    require(::lstat(finalDirectory.c_str(), &existing) != 0 && errno == ENOENT,
            "generation name already exists");
    if (::rename(staging.path().c_str(), finalDirectory.c_str()) != 0) {
        throw std::system_error(errno, std::generic_category(), "publish generation");
    }
    staging.release();
    syncDirectory(generations);
    snapshot._pageFile = pathJoin(finalDirectory, kPages);
    publishCurrent(absoluteRoot, staging.generation());
    return snapshot;
}

WebIndexSnapshot WebIndexStore::loadCurrent(const std::string &root)
{
    if (root.empty()) {
        throw std::invalid_argument("web index root must not be empty");
    }
    const std::string absoluteRoot = absoluteDirectory(root);
    std::ifstream current(pathJoin(absoluteRoot, "CURRENT").c_str(),
                          std::ios::binary);
    require(static_cast<bool>(current), "cannot open CURRENT web index generation");
    std::string generation;
    std::string extra;
    require(static_cast<bool>(std::getline(current, generation)) &&
                validGeneration(generation) && !std::getline(current, extra),
            "invalid CURRENT web index generation");
    return loadGeneration(
        pathJoin(pathJoin(absoluteRoot, "generations"), generation), generation);
}
