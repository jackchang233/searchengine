# 网页库处理模块

本模块读取模块二 XML 语料，使用 tinyxml2 提取文档、SimHash 去重，并把网页库、
网页偏移库和 TF-IDF 倒排索引作为同一个版本发布：

```bash
./bin/build_page_library
```

默认发布目录为 `data/web_index/`：

```text
data/web_index/
├── CURRENT                         当前版本名
├── PUBLISH.lock                    发布锁
└── generations/
    └── gen-xxxxxx/
        ├── webpage.dat             <doc> 片段
        ├── offset.dat              文档编号 字节偏移 字节数
        ├── inverted_index.dat      词项 文档编号 权重 [文档编号 权重]...
        └── manifest.json           数量、SHA-256、建库输入和格式版本
```

建库先写入暂存目录，计算文件大小和 SHA-256，并完整验证文档编号、字节偏移、
XML、posting 顺序和文档权重。校验成功后发布不可变版本目录，最后原子替换
`CURRENT`。构建或校验失败不会修改当前版本；若切换指针后的目录同步失败，
应重新校验 `CURRENT` 的状态。旧版本由运维保留或清理；仍在服务中
使用的版本不能提前删除。

在线侧调用 `WebIndexStore::loadCurrent(root)`：只读取一次 `CURRENT`，校验清单
和三份文件后得到只读 `WebIndexSnapshot`。快照持有网页文件描述符、内存偏移表
和倒排索引；`readDocument(id)` 按偏移读取网页。服务应在启动时加载快照并将其
共享给查询线程。

手动验证当前版本：

```bash
./bin/verify_web_index
./bin/verify_web_index /path/to/web_index
```

每个 `<item>` 最多生成一篇文档。存在 `<content>` 时采用它；缺少该标签时采用
`<description>`；两个标签都不存在时忽略。标签存在但内容为空时仍按空正文处理。

去重使用正文的 SimHash，汉明距离不超过 3 时保留先出现的文档。文件按目录扫描
结果的字典序处理，文档按 `<item>` 出现顺序处理。去重后重新连续编号，网页库的
XML 特殊字符由 tinyxml2 转义，偏移和大小按最终文件的字节数计算。

倒排索引使用“标题 + 正文”。索引前将 HTML 标签和实体替换为分词边界，只保留
汉字与 ASCII 字母（英文转小写），然后用 cppjieba Mix 模式分词，并同时过滤
中英文停用词。对词项 `t` 和文档 `d` 计算：

```text
TF(t,d)  = t 在 d 中的出现次数
IDF(t)   = log2(文档总数 / 包含 t 的文档数)
weight   = TF * IDF
weight'  = weight / sqrt(该文档所有 weight 的平方和)
```

`process()` 成功后调用 `buildInvertedIndex()`；重新调用 `process()` 会使旧倒排索引
失效，必须重新构建。词项和 posting 均按升序稳定写出。

自定义输入、版本根目录、停用词和 cppjieba 资源目录：

```bash
./bin/build_page_library xml_directory index_root stop_words_zh.txt \
    stop_words_eng.txt
./bin/build_page_library xml_directory index_root stop_words_zh.txt \
    stop_words_eng.txt jieba_dict_directory
```

对应 CMake 目标为 `searchengine_webpage`，测试包括 `page_processor_test`、
`web_index_store_test` 和 `web_searcher_test`。正式 `searchengine` 入口已接入
`WebIndexSnapshot`，通过 TLV 类型 2 提供网页查询；`search_client` 可用于本地验证。
