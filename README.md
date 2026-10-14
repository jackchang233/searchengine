# SearchEngine

基于 C++11 的搜索引擎实验项目，包含中英文关键词推荐、XML 网页离线建库和在线网页查询。网络层采用单 Reactor、非阻塞收发与有界工作线程池；客户端与服务端使用 TLV/JSON 协议。词库和网页索引分别按 generation 发布，并在启动时校验。

## 目录

- `src/reactor/`：网络层与线程池；`tests/` 为回归测试，`examples/echo/` 为独立示例。
- `src/offlineprocess/corpus/`：中英文语料处理和词库构建。
- `src/offlineprocess/webpage/`：网页提取、去重、TF-IDF 倒排索引与查询。
- `src/recommendation/`：词库版本、推荐算法和配置。
- `src/protocol/`、`src/online/`：业务协议、服务端与测试客户端。
- `doc/`：架构说明和迭代记录。

## 构建

需要 CMake、C++11 编译器、OpenSSL、tinyxml2、nlohmann/json、cppjieba、utf8cpp、simhash 及 cppjieba 词典资源。当前 CMake 会在配置阶段检查这些依赖。

```bash
cmake -S . -B build
cmake --build build -j4
```

## 数据与运行

仓库不包含大型课程资料、原始语料和生成索引。按 `conf/myconf.conf` 的路径放置或修改中英文 `.txt` 语料、停用词、网页 XML 及 cppjieba 资源，再执行：

```bash
./bin/build_corpus
./bin/build_page_library
./bin/manage_keyword_index
./bin/verify_web_index
./bin/searchengine --config conf/myconf.conf --host 127.0.0.1
```

另开终端测试：

```bash
./bin/recommendation_client --host 127.0.0.1 --port 8888 --query helo
./bin/search_client --host 127.0.0.1 --port 8888 --query 中国
```

完整测试使用 `ctest --test-dir build --output-on-failure`；涉及真实语料/索引的测试需要先准备数据。词库生成和回滚详见 [语料模块说明](src/offlineprocess/corpus/README.md)，网页索引详见 [网页模块说明](src/offlineprocess/webpage/README.md)。
