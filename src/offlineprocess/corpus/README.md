# 语料处理模块

本模块处理模块一的中英文语料，生成词典和字符索引。正常完整建库使用统一入口：

```bash
./bin/build_corpus
```

单语言入口继续用于局部开发和排错：

```bash
./bin/build_english_keywords
./bin/build_chinese_keywords
```

默认输入与输出：

| 程序 | 输入 | 输出 |
| --- | --- | --- |
| `build_corpus` | 中英文语料、两份停用词及 cppjieba 资源 | `data/keyword_index/generations/<generation>/` 下的四份词典 / 索引及 `manifest.json`；切换 `CURRENT` |
| `build_english_keywords` | `docs/模块一/yuliao/english/` 与英文停用词 | `data/en_dict.dat`、`data/en_index.dat` |
| `build_chinese_keywords` | `docs/模块一/yuliao/chinese/` 与中文停用词 | `data/cn_dict.dat`、`data/cn_index.dat` |

`build_corpus` 先扫描两类 `.txt` 文件并在内存中完成两种语言的处理；只有两者均成功，
才写入同一个临时 generation。写出后严格加载四文件，记录大小、SHA-256、词条数与输入
指纹并校验，最后原子切换 `CURRENT`。失败不会切换当前版本；旧版本仍保留。
正式服务和推荐 CLI 从配置中的 `keyword_index_root` 加载当前版本，启动后不会热更新。
单语言程序仍写入旧的独立文件，不会切换 `CURRENT`。

英文按连续 ASCII 字母分词并转为小写。中文先规范分隔符，再使用 cppjieba Mix
模式分词；两者都过滤对应停用词。字符索引中的编号是该词典的零基行号。

中文处理使用 `Utf8Utils` 校验和拆分 Unicode 码点。分词器默认读取 CMake 检测到
的 `/usr/local/dict/`，也可以在命令行传入自定义 cppjieba 资源目录。

自定义中文输入和输出：

```bash
./bin/build_chinese_keywords corpus_directory stop_words_zh.txt dictionary.dat index.dat
./bin/build_chinese_keywords corpus_directory stop_words_zh.txt dictionary.dat index.dat jieba_dict_directory
```

统一入口的完整自定义形式：

```bash
./bin/build_corpus --index-root data/keyword_index
./bin/build_corpus \
  english_corpus_directory chinese_corpus_directory \
  stop_words_eng.txt stop_words_zh.txt \
  index_root \
  [jieba_dict_directory]
```

旧的四输出路径参数形式仍可用于兼容测试，但不具备跨四文件的原子发布语义。

检查当前版本与手动回滚（回滚后需重启在线服务）：

```bash
./bin/manage_keyword_index [index_root]
./bin/manage_keyword_index --activate gen-XXXXXX [index_root]
```

`gen-XXXXXX` 必须是 `generations/` 下已存在且校验通过的版本名；回滚只切换指针，
不删除任何版本。不要在服务运行时修改其已加载版本内的文件。

对应 CMake 目标为 `searchengine_corpus`，统一可执行程序目标为 `build_corpus`。主要测试是
`keyword_processor_test`、`chinese_keyword_processor_test` 和 `dictionary_store_test`。
