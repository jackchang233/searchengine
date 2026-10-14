# 离线处理模块

`offlineprocess` 按业务职责分为两个模块，共享目录扫描与 UTF-8 工具：

```text
offlineprocess/
├── corpus/                 中英文语料、词典和字符索引
├── webpage/                XML 网页提取、去重、网页库、偏移库和倒排索引
├── DirectoryScanner.*      两个模块共享的递归文件扫描
└── Utf8Utils.*             两个模块共享的 UTF-8 码点工具
```

CMake 目标及依赖：

```text
searchengine_corpus  ──┐
                       ├──> searchengine_offline_common
searchengine_webpage ──┘
```

两个业务模块没有相互链接。语料模块负责产生关键词词典与字符索引，并经
`DictionaryStore` 按版本发布；网页模块负责产生网页库、字节偏移库与 TF-IDF
倒排索引，并经 `WebIndexStore` 按版本发布和校验加载。
公共目标只提供无业务状态的基础工具。

默认完整离线构建入口：

```bash
./bin/build_corpus
./bin/build_page_library
```

从项目根目录构建并测试：

```bash
cmake -S . -B build
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

模块说明：

- [语料处理](corpus/README.md)
- [网页库处理](webpage/README.md)
