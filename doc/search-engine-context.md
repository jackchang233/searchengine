# C++ Search Engine 项目上下文：当前实现与长期演进

修订日期：2026-10-11  
修订依据：当前仓库源码、CMake、配置及离线产物，以及同目录的[搜索引擎项目迭代方案](搜索引擎项目迭代方案.md)。

本文保留原有 Reactor、全文检索、Hybrid Retrieval、RAG、MCP 和 Agent 的长期方向，并将其与当前已实现的代码分开。网页 TF-IDF 离线索引、TLV 在线网页查询和关键词词库整批发布均已完成；下一步需评估检索效果、长时负载和外部接入。

文中“当前实现”来自实际源码；“本轮拟实现”是近期任务；“后续可选”是长期规划。除明确标注为当前接口的代码外，其他代码和架构图均为设计示意。后续修改时，以重新读取的源码为事实依据，不能把规划中的类名当作已存在接口。

## 0. 当前仓库基线与文档对照

### 0.1 当前已实现能力

| 范围 | 当前代码 | 已实现内容 |
| --- | --- | --- |
| 语言与构建 | `CMakeLists.txt` | C++11、按目标构建、独立示例和测试入口 |
| 目录扫描 | `src/offlineprocess/DirectoryScanner.*` | 递归扫描普通文件，绝对路径、排序、忽略符号链接 |
| 语料统一入口 | `src/offlineprocess/corpus/BuildCorpus.cpp` | 一次编排中英文处理并按 generation 发布四份词典和索引 |
| 词库版本仓库 | `src/recommendation/DictionaryStore.*`、`bin/manage_keyword_index` | 四文件清单、SHA-256 校验、原子切换 CURRENT 和手动回滚 |
| 英文离线建库 | `src/offlineprocess/corpus/KeywordProcessor.*` | ASCII 分词、小写、停用词过滤、词频和字母索引 |
| 中文离线建库 | `src/offlineprocess/corpus/ChineseKeywordProcessor.*` | cppjieba Mix 分词、过滤、词频和 Unicode 字符索引 |
| 字符处理 | `src/offlineprocess/Utf8Utils.*` | 通过 utf8cpp 校验 UTF-8、拆分字符、转换码点 |
| 网页离线索引 | `src/offlineprocess/webpage/PageProcessor.*` | tinyxml2 提取、SimHash 去重、网页/偏移库、正文清洗和归一化 TF-IDF 倒排索引 |
| 网页版本仓库 | `src/offlineprocess/webpage/WebIndexStore.*` | 三份产物按 generation 发布、SHA-256 与跨文件校验、只读快照加载 |
| 在线网页检索 | `src/offlineprocess/webpage/TextAnalyzer.*`、`WebQueryAnalyzer.*`、`WebSearcher.*` | 共用清洗/分词规则、严格交集召回、TF-IDF 余弦排序、摘要 |
| TCP 框架 | `src/reactor/` | 单 Reactor、epoll LT、连接缓冲、非阻塞收发 |
| 跨线程回传 | `EventLoop.*`、`TcpConnection.*` | `runInLoop`、`queueInLoop`、eventfd、`sendInLoop` |
| 工作线程 | `ThreadPool.*`、`TaskQueue.*` | 有界任务队列、非阻塞提交结果，停止后排空已提交任务 |
| 配置与词库 | `src/recommendation/` | 强类型配置、配置文件相对路径、双词库严格加载与只读候选查询 |
| 推荐算法与 CLI | `src/recommendation/`、`bin/recommend_keywords` | 归一化、Unicode 编辑距离、完整排序 Top K、结构化命令行输出 |
| 帧与业务协议 | `src/protocol/` | 行帧/TLV 编解码、类型 1 推荐和类型 2 网页查询的 JSON 请求/响应 |
| 在线服务 | `src/online/`、`bin/searchengine`、`bin/search_client` | 两类 TLV 请求共用在途编号、非阻塞提交、半关闭回传、超时排空及 SIGINT/SIGTERM 停服；Echo 另有独立示例 |
| 测试 | `CMakeLists.txt` | 注册线程池、非阻塞提交、词库加载与版本发布、推荐算法与 CLI、目录扫描、语料、网页处理、两类协议、连接分帧和在线服务等 17 个测试 |

当前 `data/` 中存在网页和关键词词库的版本产物，也保留旧的中英文词典文件；下表中的旧四文件仅用于兼容入口，正式配置加载 `keyword_index_root`：

| 产物 | 记录数 | 当前作用 |
| --- | ---: | --- |
| `data/en_dict.dat` | 34,705 | 英文词语与词频 |
| `data/en_index.dat` | 26 | 字母到英文词典零基编号 |
| `data/cn_dict.dat` | 17,315 | 中文词语与词频 |
| `data/cn_index.dat` | 3,084 | 字符到中文词典零基编号 |
| `data/keyword_index/generations/<generation>/` | 四文件及 manifest | 正式中英文词库快照 |
| `data/keyword_index/CURRENT` | 一个版本名 | 原子选择当前四份词库产物 |
| `data/web_index/generations/<generation>/webpage.dat` | 3,841 篇文档 | 去重后的 `<doc>` 网页库 |
| `data/web_index/generations/<generation>/offset.dat` | 3,841 行 | 文档编号、字节偏移和字节数 |
| `data/web_index/generations/<generation>/inverted_index.dat` | 97,061 个词项、1,048,882 条 posting | 归一化 TF-IDF 权重 |
| `data/web_index/CURRENT` | 一个版本名 | 原子选择当前三份网页产物 |

这些数字来自当前词典文件与网页版本快照。本次网页语料共扫描 37 个 XML 文件和 4,365 个 `<item>`：4,315 项含 `content` 或 `description`，50 项因两者都缺失而忽略，SimHash 汉明距离不超过 3 的 474 项被判为重复。网页索引在标题与去标签正文上分词，同时过滤中英文停用词，按 `TF * log2(N/DF)` 计算并逐文档 L2 归一化。字符索引仍指向词典词条，不是网页或文档 ID；语料词频和网页 TF-IDF 都不是用户点击热度或 BM25 分数。

### 0.2 当前未实现能力

- HTTP 消息解析与 HTTP Router；正式 `SearchEngineServer` 已采用 TLV/JSON，不是 HTTP 服务。
- `Channel`、独立 `Poller`、`EventLoopThreadPool` 和 Multi-Reactor。
- Echo 示例仍使用队列满时会等待的 `addTask()`；正式服务使用非阻塞提交。长时压力测试和服务内部队列等待耗时采样尚未完成。
- BM25、并集召回、网页索引在线热更新和标注查询集；当前网页查询使用严格交集与 TF-IDF 余弦排序，启动时加载并校验快照。
- LRU、Redis、MySQL、向量检索、RAG、MCP Server、Agent 和分布式索引。

这些能力不能从配置键、资料包或规划类名推断为已实现。`SocketIO` 辅助类虽然存在，当前 `TcpConnection` 的事件处理直接使用非阻塞 `recv/send` 与自身字符串缓冲，不经由它执行阻塞读取。

### 0.3 与迭代方案的差异及采纳结论

| 主题 | 原上下文的方向 | 当前代码 / 近期方案 | 本次修订结论 |
| --- | --- | --- | --- |
| 首个业务目标 | 全文检索、BM25、HTTP | 已有关键词词典与字符索引 | 先完成推荐闭环，再做文档检索 |
| 网络模型 | Main Reactor + 多 IO Reactor | 一个 EventLoop + Worker Pool | 复用单 Reactor，压测后决定是否扩展 |
| 协议 | HTTP 优先 | Echo 为行帧，通用网络层也已支持 TLV | 正式推荐服务选择 TLV；HTTP 后续作为独立适配层 |
| 唤醒机制 | 建议增加 eventfd | 已有 eventfd 与 pending functors | 不重复实现，重点补负载和生命周期控制 |
| 字符工具 | 描述 UTF-8 转码点 | 已有 `Utf8Utils` | 复用并提取公共 CMake 目标 |
| 共享索引 | `std::shared_mutex` 示例 | C++11，词库启动后只读 | 使用只读快照；更新时短暂交换共享指针 |
| 词库编号 | 未明确来源编号 | 中英文分别从 0 编号 | 采用 `(sourceId, wordId)`，不直接拼接索引 |
| 文档排序 | 直接规划 BM25 | TF-IDF 离线基线已生成 | 先接入余弦查询，再用同一数据比较 BM25 |
| 数据层 | MySQL + Redis + 各类索引 | 当前是本地文件 | 文件先满足闭环，缓存和数据库按需求增加 |
| AI 接入 | Hybrid、RAG、MCP、Agent | 尚无文档检索能力 | 保留为长期方向，不占用本轮交付范围 |

发生冲突时，当前能力以源码为准，近期执行步骤以迭代方案为准，长期方向参考本文后续章节。两份文档中的接口和限额仍需在实际实现阶段验证，不能当作当前行为。

### 0.4 当前代码的真实调用关系

```text
src/reactor/examples/echo/TestEchoServer.cc
    → EchoServer
        → TcpServer
            → Acceptor + EventLoop
                → TcpConnection
                    → 按 Echo 选择的行帧策略提取完整 Frame
        → ThreadPool
            → MyTask::process()
                → 构造 echo 响应
                → TcpConnection::sendInLoop()
                    → EventLoop::runInLoop()
                    → eventfd 唤醒，Reactor 实际发送

src/online/Main.cpp
        → SearchEngineServer → Configuration + DictionaryRepository + WebIndexSnapshot
        → TcpServer(TLV) → TcpConnection::dispatchFrames()
            → RecommendationProtocol / SearchProtocol::decodeRequest()
            → 在途编号登记 → ThreadPool::tryAddTask()
                → KeywordRecommender::recommend() / WebSearcher::search()
                → TcpConnection::postToLoop()
                    → Reactor 清理编号并发送 TLV 响应
```

当前接口名称如下（省略实例和声明，只列调用名称），不要用未来示意中的名字替代：

```cpp
KeywordProcessor::processEnglish(corpusFiles, stopWordsFile);
KeywordProcessor::storeEnglish(dictionaryFile, indexFile);

ChineseKeywordProcessor::process(corpusFiles, stopWordsFile);
ChineseKeywordProcessor::store(dictionaryFile, indexFile);

PageProcessor::process(xmlFiles);
PageProcessor::store(pageLibraryFile, offsetLibraryFile);
PageProcessor::buildInvertedIndex(stopWordFiles);
PageProcessor::storeInvertedIndex(invertedIndexFile);
WebIndexStore::publish(processor, indexRoot, inputs);
WebIndexStore::loadCurrent(indexRoot);

ServerOptions options = Configuration::load(configFile);
DictionaryRepository::load(sources);
QueryNormalizer::normalize(query, options.maxQueryChars);
EditDistance::levenshtein(left, right);
KeywordRecommender(repository, options.maxQueryChars, options.maxTopK)
    .recommend(query, topK);

Utf8Utils::splitCharacters(text);
Utf8Utils::toCodePoints(text);

ThreadPool::addTask(task);        // 当前接口，队列满可能等待
ThreadPool::tryAddTask(task);     // 当前非阻塞提交接口，返回 SubmitResult
TlvFrameCodec::decode(bytes, maxBodyBytes, FrameDirection::RequestsOnly);
RecommendationProtocol::decodeRequest(frame, limits);
TcpConnection::sendInLoop(data); // 当前已支持跨线程回传
TcpConnection::postToLoop(callback); // 当前可投递连接关联的完成回调
```

`getLoop()`、`setRequestCallback()`、`workerPool.submit()`、`searchEngine.search()` 等名字在原讨论中是示意接口，当前仓库没有对应实现。

### 0.5 最近的开发顺序

网页离线分支当前为：

```text
XML item 提取 → content / description 选择 → SimHash 去重
    → 网页库 + 字节偏移库
    → 标题/正文清洗与分词 → TF-IDF → 词项到文档倒排索引（已完成）
    → 版本校验与加载（已完成）
    → 查询向量 → 候选交集与余弦排序 → TLV 类型 2（已完成）
```

在线推荐分支仍按以下顺序推进：

```text
提取 UTF-8 公共目标（已完成）
    → Configuration + DictionaryRepository（已完成）
    → EditDistance + KeywordRecommender + 命令行工具（已完成）
    → TLV 帧协议与请求编号字段校验（已完成）
    → 非阻塞任务提交接口 + SearchEngineServer + TLV 客户端（已完成）
    → 半关闭、停服、慢读与首份性能基线验收（已完成）
    → 性能测量，再决定缓存、HTTP 和 Multi-Reactor
```

中文语料当前扫描 `docs/模块一/yuliao/chinese/`，停用词在 `docs/模块一/yuliao/stop_words/`。在线配置的相对数据路径以配置文件目录为基准；现有建库入口的默认相对路径仍以运行工作目录为基准。

------------------------------------------------------------------------

## 1. 项目定位

最初的问题是：在大模型和 Agent 快速发展的背景下，继续使用 C++
实现搜索引擎是否已经落后。

结论：

项目当前以 C++ 网络框架和中文检索基础为开发主线；完成课程要求的可验证闭环后，再按明确的评测和接入需求扩展。

长期可选定位是：

> **面向 RAG / Agent 的 C++ 检索服务，逐步扩展为 Hybrid Search Engine。**

“高性能”需由真实基准证明，Hybrid / RAG / Agent 当前均为规划，不能写成已经交付的项目能力。

项目主体仍然应该体现 C++ 后端与检索系统能力：

-   C++ / Linux
-   Socket / epoll
-   Reactor
-   多线程 / 线程池
-   HTTP / RPC
-   中文 Query Processing
-   倒排索引
-   BM25
-   Top-K
-   Redis
-   MySQL
-   性能优化
-   并发与索引管理

在此基础上逐步扩展：

-   Embedding
-   Vector Search
-   Hybrid Retrieval
-   Rerank
-   RAG
-   MCP Server
-   Agent

核心原则：

> **底层搜索系统做扎实，上层再接入 LLM / RAG / MCP /
> Agent。不要把项目变成单纯的 LLM API 调用器。**

------------------------------------------------------------------------

## 2. 推荐演进路线

版本编号表达开发顺序，不表示已发布版本。当前完成的是 V0 的离线建库及网络基础；V1 的详细任务、接口与验收以《搜索引擎项目迭代方案》为准。

### V0：当前基线——离线关键词建库与 Echo

```text
中英文语料 + 停用词
        ↓
KeywordProcessor / ChineseKeywordProcessor
        ↓
en_dict + en_index / cn_dict + cn_index

客户端 → 单 EventLoop → 行协议 Echo → ThreadPool → sendInLoop
```

中文建库使用 cppjieba，字符校验和拆分使用 utf8cpp。两套词库独立编号；网络示例还没有接入这些数据。

### V1：下一轮——在线关键词推荐

```text
配置与双词库加载
       ↓
查询归一化与 Unicode 码点转换
       ↓
字符索引候选并集 / 去重
       ↓
编辑距离 + 词频 + 字典序排序
       ↓
命令行推荐工具
       ↓
单 Reactor + 非阻塞任务提交 + TLV / JSON 推荐服务
```

目标是跑通相似词推荐，不是全文检索。排序按“编辑距离升序、词频降序、词条字典序升序”；候选不足 K 时不补足。没有共享字符时返回空候选，首版不保证全词典范围的全局最近邻。

推荐与网页查询的算法、CLI、TLV/JSON 协议和在线服务已独立验证；半关闭、停服排空和慢读场景已有测试，[推荐](推荐服务性能基线.md)与[网页查询](网页搜索性能基线.md)两份本机性能基线已记录。词库四文件整批发布与手动回滚也已完成。下一步可推进网页检索效果评测和长时负载测试；缓存仍需以更充分的基线测量为依据，不作为启动依赖。

### V2：后续——文档 / 网页全文检索

``` text
Document
   ↓
Tokenizer
   ↓
Inverted Index
   ↓
TF-IDF / 余弦相似度基线
   ↓
Top-K
   ↓
网页查询服务（先复用 TLV，HTTP 后续适配）
```

目标：

-   建立文档索引
-   从模块二已有 XML 语料提取正文和元信息
-   中文分词与停用词过滤
-   网页偏移库、词项到文档的倒排索引
-   TF-IDF / 余弦相似度基线，之后对照测试 BM25
-   Top-K 返回
-   网页查询与摘要返回

字符到词条的推荐索引不能直接当作词项到文档的网页倒排索引。两条业务可以共享网络框架和公共工具，但数据结构、评分与返回类型应独立。

### V3：可选工程化——缓存、HTTP 与 Multi-Reactor

以测量结果和接入需求决定是否实施，不因目标图中出现这些组件就提前重写网络库。

``` text
C++ / Linux
     ↓
Multi-Reactor
     ↓
epoll
     ↓
HTTP
     ↓
Worker Thread Pool
     ↓
Search Service
     ↓
Redis / MySQL / Index
```

目标：

-   网络 IO 与检索计算解耦
-   支持并发连接
-   搜索任务进入 Worker Pool
-   Redis 做热点查询缓存
-   MySQL 或其他存储保存文档 Metadata

### V4：Hybrid Retrieval（长期可选）

``` text
                 Query
                   │
          ┌────────┴────────┐
          ↓                 ↓
       BM25              Embedding
          │                 │
   Inverted Index       Vector Index
          │                 │
        Top-K             Top-K
          └────────┬────────┘
                   ↓
             Fusion / RRF
                   ↓
                Rerank
                   ↓
                 Top-K
```

目标：

-   保留关键词精确检索
-   增加向量语义检索
-   融合 BM25 与 Vector Search
-   增加 Reranker

### V5：RAG（长期可选）

``` text
Question
   ↓
Hybrid Retrieval
   ↓
Top-K Chunks
   ↓
Rerank
   ↓
Context
   ↓
LLM
   ↓
Answer
```

搜索引擎负责 Retrieval，LLM 负责生成答案。

### V6：MCP / Agent（长期可选）

``` text
User
 ↓
LLM / Agent
 ↓
MCP Client
 ↓
MCP Server
 ↓
Search Service
 ↓
Hybrid Search Engine
 ↓
Top-K
 ↓
RAG / Agent
```

MCP 是 Agent 的标准化接入层。

### V7：分布式搜索（长期可选）

``` text
                 Gateway
                    │
       ┌────────────┼────────────┐
       ↓            ↓            ↓
 Search Node 1 Search Node 2 Search Node 3
       │            │            │
     Index        Index        Index
       │            │            │
       └────────────┼────────────┘
                    ↓
                Aggregator
                    ↓
                  Top-K
```

可以进一步体现：

-   RPC
-   服务发现
-   Index Sharding
-   并行查询
-   聚合排序
-   负载均衡
-   分布式缓存

------------------------------------------------------------------------

## 3. Reactor 在搜索引擎中的定位

当前实现为 `TcpServer` 持有一个 `EventLoop`，由该循环同时处理接入、读写和完成回调。`EventLoop` 直接管理 epoll 与连接，没有 `Channel`、`Poller`、`Buffer` 或 `EventLoopThreadPool` 类。下图是长期分层示意，不是当前类图；近期无需为了匹配图形创建这些类。

Reactor 不应该被"改造成搜索算法"。

正确定位：

> **Reactor 是搜索服务的底层高并发运行框架；SearchEngine
> 是核心检索能力。**

整体关系：

``` text
                         Search Server
                              │
              ┌───────────────┴───────────────┐
              │                               │
         Reactor Layer                   Search Layer
              │                               │
    ┌─────────┼─────────┐          ┌──────────┼──────────┐
    │         │         │          │          │          │
 Acceptor  EventLoop  Channel   QueryParser  Index    Ranker
    │         │         │          │          │          │
    └─────────┼─────────┘          └──────────┼──────────┘
              │                               │
           HTTP/RPC                       BM25 / TopK
              │                               │
              └───────────────┬───────────────┘
                              │
                        SearchService
```

典型 Reactor：

``` text
              Server
                │
             Acceptor
                │
             accept()
                │
          ┌─────▼─────┐
          │ EventLoop │
          └─────┬─────┘
                │
              epoll
                │
       ┌────────┼────────┐
       ▼        ▼        ▼
    Channel  Channel  Channel
       │        │        │
      Conn     Conn     Conn
```

基础网络模块可能包括：

``` text
net/
├── EventLoop
├── Poller / EPollPoller
├── Channel
├── TcpConnection
├── TcpServer
├── Acceptor
├── Buffer
├── EventLoopThread
└── EventLoopThreadPool
```

------------------------------------------------------------------------

## 4. Reactor 改造原则：网络层与业务层解耦

当前对应函数是 `TcpConnection::handleReadEvent()`；下面的 `handleRead()`、`readBuffer()`、`setRequestCallback()` 是概念伪代码，不是仓库已有 API。

不要把编辑距离、词库检索或外部存储访问直接写进读事件处理。现有 `TcpConnection::dispatchFrames()` 已通过显式行帧/TLV 策略提取完整帧，并在调用 `TcpFrameCallback` 前消费字节；在线服务回调解析推荐 JSON、登记请求并非阻塞提交，推荐计算由工作线程执行。

错误方向：

``` cpp
void TcpConnection::handleRead()
{
    // read socket
    // 中文分词
    // 编辑距离
    // 查询倒排索引
    // BM25
    // Redis
    // JSON
}
```

应该通过 Callback 解耦：

``` cpp
void TcpConnection::handleRead()
{
    readBuffer();
    requestCallback_(shared_from_this(), inputBuffer_);
}
```

上层：

``` cpp
server.setRequestCallback(
    [&](const TcpConnectionPtr& conn,
        const std::string& request)
    {
        searchService.handle(conn, request);
    }
);
```

形成：

``` text
Reactor
   │
   │ Callback
   ▼
Service / Business Layer
```

------------------------------------------------------------------------

## 5. HTTP 接入层（后续适配，不是本轮协议）

当前 Echo 使用行帧；正式推荐服务使用 TLV 并以 JSON 为正文。TLV 头固定 8 字节：网络字节序的 `uint32_t type` 与 `uint32_t length`；长度是正文 UTF-8 字节数，不能发送 C++ 对象的内存布局。具体错误码、字段与拆包状态见迭代方案第 6 节。

HTTP 是面向浏览器或通用客户端的后续适配层，与 TLV 共用推荐业务，不替代首轮验收，也不能直接接收 TLV 字节。以下 HTTP 类和 Router 都尚未实现。

例如：

``` http
GET /search?q=epoll&page=1
```

请求链：

``` text
Socket
  ↓
TcpConnection
  ↓
Buffer
  ↓
HttpParser
  ↓
HttpRequest
  ↓
Router
  ↓
SearchController
```

HTTP 模块：

``` text
http/
├── HttpRequest
├── HttpResponse
├── HttpParser
├── HttpContext
├── HttpRouter
└── HttpServer
```

概念代码：

``` cpp
router.GET("/search",
    [&](const HttpRequest& req, HttpResponse& resp) {
        auto query = req.getParam("q");
        auto result = searchService.search(query);
        resp.setJson(result);
    }
);
```

------------------------------------------------------------------------

## 6. 线程模型：当前单 Reactor，按需演进 Multi-Reactor

当前在线推荐服务使用单 Reactor + Worker Pool：

```text
EventLoop：收发、提取 TLV、解析受限 JSON、登记请求编号
       ↓ tryAddTask（正式服务已接入；队列满立即拒绝）
Worker：查询归一化、候选检索、编辑距离、排序、响应编码
       ↓ queueInLoop / eventfd
EventLoop：撤销在途登记、发送响应、推进连接生命周期
```

首版请求正文上限建议 4 KiB，查询不超过 64 个码点。JSON 解析在 Reactor 执行这一选择只适用于受限小请求，需测量耗时；不能把整句分词、推荐计算或阻塞式缓存访问放到 Reactor。

`TaskQueue::push()` 在队列满时等待，`ThreadPool::addTask()` 因此可能阻塞网络线程。现有 `tryPush()` / `tryAddTask()` 在同一把队列锁内检查容量并插入，不等待空位；结果为 `Accepted`、`QueueFull`、`Stopped` 或 `InvalidTask`。已经接纳的任务在停止时排空。Echo 仍调用阻塞接口，正式在线服务已改用非阻塞接口并处理拒绝结果。先调用 `full()` 再调用阻塞 `push()` 不能保证非阻塞。

下方 Multi-Reactor 图仅是性能瓶颈出现后的扩展。当前没有独立 Main Reactor、IO Reactor 线程池或连接迁移机制。

最重要的性能原则：

> **不要在 EventLoop 线程中执行耗时搜索。**

错误流程：

``` text
epoll_wait
    ↓
收到请求
    ↓
分词
    ↓
编辑距离
    ↓
倒排索引
    ↓
BM25
    ↓
Top-K
    ↓
返回
    ↓
epoll_wait
```

如果一次搜索耗时 20ms，那么对应 EventLoop 可能在这 20ms
内无法及时服务其他连接。

后续可选：

``` text
                  Main Reactor
                       │
                   accept()
                       │
              ┌────────┴────────┐
              ▼                 ▼
         IO Reactor        IO Reactor
              │                 │
           epoll              epoll
              │                 │
              └────────┬────────┘
                       │
                HTTP Request
                       │
                       ▼
               ┌──────────────┐
               │ Worker Pool  │
               └───────┬──────┘
                       │
                 Search Task
                       │
                       ▼
                Search Engine
                       │
                  SearchResult
                       │
                       ▼
                  EventLoop
                       │
                    send()
```

未来 Multi-Reactor 的职责分配：

| 线程 | 职责 |
| --- | --- |
| Main Reactor | accept 新连接并分配给 IO Reactor |
| IO Reactor | 所属连接的 socket 读写、协议解析、完成回调 |
| Search Worker | 推荐或文档检索计算，不操作连接缓冲 |
| Background Worker | 后续建索引、更新、合并、统计 |

------------------------------------------------------------------------

## 7. Worker 如何安全返回结果

Worker 不应该直接随意操作属于 IO Thread 的 socket。

推荐：

``` text
Worker
   │
 search()
   ↓
SearchResult
   │
   ▼
queueInLoop()
   │
   ▼
IO EventLoop
   │
   ▼
TcpConnection::send()
```

当前 `MyTask` 已通过 `conn->sendInLoop(message)` 回传 Echo；该函数捕获连接的 `shared_ptr` 并通过所属循环调度，实际发送前检查连接状态。无需为了回传先添加当前不存在的 `getLoop()`。

推荐业务任务持有只读词库资源，可用 `weak_ptr` 引用连接。结果完成后，向 Reactor 投递一个统一完成回调，在该回调内处理响应发送、在途计数和请求编号集合。仅调用 `sendInLoop()` 不能自动完成这些业务状态更新。

连接对象仍存在不等于 socket 仍可写；断开后丢弃响应，成功和异常路径都需要释放在途登记。多请求可乱序完成，客户端依靠 `request_id` 匹配，不依赖响应顺序。

EventLoop 当前已经支持：

``` cpp
void EventLoop::runInLoop(Functor cb);
void EventLoop::queueInLoop(Functor cb);
```

并维护受互斥锁保护的待执行任务。以下是类型示意，成员的实际命名以源码为准：

``` cpp
std::vector<std::function<void()>> pendingFunctors_;
```

正式在线服务已补齐两处生命周期行为：

- `handleReadEvent()` 遇到 `recv() == 0` 后将读方向标记关闭。完整请求的响应继续回传，在途任务和输出排空后关闭；残留半帧则关闭连接。若有待发输出，写事件继续保留。
- `SearchEngineServer::stop()` 请求 Reactor 进入 `Draining`，停止新连接与新业务任务；在途任务和输出缓冲排空或期限到达后关闭连接，再由管理线程等待工作线程退出。Echo 示例仍保留自身的停止实现。

连接排空超时只允许关闭慢连接，不意味着 C++11 可以安全强杀仍运行的线程。完整状态机、所有权和验收见迭代方案第 7、10 节。

------------------------------------------------------------------------

## 8. eventfd：跨线程唤醒 EventLoop

这部分已经实现，不是下一轮待补模块。应保留现有 `queueInLoop()`、唤醒 fd 和待执行回调流程，并增加与推荐任务回传相关的回归测试。

如果 IO EventLoop 正阻塞在 `epoll_wait()`，Worker
将任务放入队列后需要唤醒它。

使用 Linux `eventfd`：

``` text
Worker Thread
     │
 queueInLoop()
     │
     ▼
pendingFunctors
     │
     ▼
write(eventfd)
     │
     ▼
唤醒 epoll_wait
```

EventLoop：

``` text
epoll_wait()
    ↓
eventfd readable
    ↓
handleWakeup()
    ↓
doPendingFunctors()
```

当前 EventLoop 管理：

``` text
EventLoop
   │
   ├── socket fd
   └── eventfd
```

`timerfd` 尚未实现。若未来有超时 / 定时清理需求再引入，不将它写作当前能力。

------------------------------------------------------------------------

## 9. SearchEngine 核心结构

近期业务入口已实现为 `KeywordRecommender`，依赖 `DictionaryRepository`、`QueryNormalizer` 和 `EditDistance`，并已接入 `SearchEngineServer`。单词推荐直接对码点序列计算，不调用 cppjieba。

下方结构包含文档检索的长期扩展示意，其中 BM25、缓存和外部接口仍未实现。当前网页查询已有分词、词项到文档的索引、余弦评分和摘要；不能把推荐结果当作网页搜索结果。

长期示意：

``` text
SearchEngine
│
├── QueryProcessor
├── Tokenizer
├── SpellCorrector
├── InvertedIndex
├── BM25Ranker
├── TopK
├── DocumentManager
│
├── VectorSearchEngine   # 后续
├── HybridRanker         # 后续
└── Reranker             # 后续
```

概念：

``` cpp
class SearchEngine {
public:
    SearchResult search(const SearchQuery& query);

private:
    Tokenizer tokenizer_;
    InvertedIndex index_;
    BM25Ranker ranker_;
    SpellCorrector corrector_;
    Cache cache_;
};
```

基础查询：

``` text
Query
 ↓
UTF-8 Decode
 ↓
中文分词
 ↓
Query Correction
 ↓
倒排索引
 ↓
候选文档
 ↓
BM25
 ↓
Top-K
 ↓
摘要
 ↓
SearchResult
```

------------------------------------------------------------------------

## 10. 中文最小编辑距离

`Utf8Utils::toCodePoints()` 已实现，但编辑距离模块尚未实现。下一轮使用普通单位代价 Levenshtein，输入是 Unicode 码点，不是 UTF-8 字节，也不是分词结果。

Levenshtein Distance 使用：

-   插入
-   删除
-   替换

定义：

``` text
dp[i][j]
```

表示 A 的前 i 个字符转换成 B 的前 j 个字符所需的最小操作数。

状态转移：

``` text
如果 A[i-1] == B[j-1]:

dp[i][j] = dp[i-1][j-1]

否则:

dp[i][j] =
min(
    dp[i-1][j]   + 1,   // 删除
    dp[i][j-1]   + 1,   // 插入
    dp[i-1][j-1] + 1    // 替换
)
```

初始化：

``` text
dp[i][0] = i
dp[0][j] = j
```

时间复杂度：

``` text
O(mn)
```

空间可以从：

``` text
O(mn)
```

优化为：

``` text
O(min(m,n))
```

------------------------------------------------------------------------

## 11. 中文 UTF-8 注意事项

不能直接使用 UTF-8 `std::string` 的下标作为"中文字符"。

例如：

``` cpp
std::string text = "搜索引擎";
```

UTF-8 中汉字通常占多个字节，直接使用 `text[i]` 比较的是 byte。

推荐：

``` text
UTF-8 std::string
        ↓
Unicode Code Point
        ↓
std::u32string
        ↓
Edit Distance
```

接口：

``` cpp
int editDistance(
    const std::u32string& a,
    const std::u32string& b);
```

空间优化 DP 核心：

``` cpp
std::vector<int> prev(n + 1);
std::vector<int> curr(n + 1);

for (size_t j = 0; j <= n; ++j)
    prev[j] = static_cast<int>(j);

for (size_t i = 1; i <= m; ++i) {
    curr[0] = static_cast<int>(i);

    for (size_t j = 1; j <= n; ++j) {
        if (a[i - 1] == b[j - 1]) {
            curr[j] = prev[j - 1];
        } else {
            curr[j] = std::min({
                prev[j] + 1,      // delete
                curr[j - 1] + 1,  // insert
                prev[j - 1] + 1   // replace
            });
        }
    }

    std::swap(prev, curr);
}
```

------------------------------------------------------------------------

## 12. Query Correction：先候选召回，再计算距离

当前词库共 52,020 条记录，不是下面假设的百万词规模。全量遍历可以作为小型测试词典的正确性参考，但在线基线复用现有字符索引：对归一化查询的不同字符，在中英文库中取 postings 并集，再按 `(sourceId, wordId)` 去重。

两套词库都参与检索，不强制把查询划分为纯英文或纯中文。缺失的字符索引跳过，不能抛出查找异常。候选库中的词条在加载时预先转换为码点；请求内使用局部计算状态，避免多线程共享可变 DP 数组。

候选按“距离升序、频率降序、词条字典序升序”排序，先用完整排序建立可验证基线，再改有界堆。若用长度差剪枝，只有下界严格大于当前最差距离才能跳过；距离相等仍可能在词频或字典序上更优。

字符并集不等于完整最近邻召回。全错且没有共享字符时返回空结果；热门单字符也可能召回大量词。不能任意截断 postings 并声称保持结果等价。拼音、N-Gram、形近字属于后续召回策略，须提供专门评测再接入。

如果词典有 100 万个词，不应该：

``` text
query
 ↓
word1 → Edit Distance
word2 → Edit Distance
...
word1000000 → Edit Distance
```

应该：

``` text
用户 Query
     ↓
Candidate Recall
     ↓
少量候选词
     ↓
Edit Distance
     ↓
Candidate Ranking
     ↓
Query Rewrite
```

中文候选召回可以考虑：

-   字符索引
-   拼音索引
-   N-Gram
-   同音字
-   形近字

------------------------------------------------------------------------

## 13. 中文加权编辑距离（长期扩展）

当前没有拼音、形近字资源或对应算法。首版仍用整数距离与确定排序。以下 0.2 / 0.4 / 0.6 权重是讨论示例，不是已验证参数；引入后需版本化排序规则，并比较纠错标注集上的准确率与延迟。

普通编辑距离中：

``` text
replaceCost = 1
```

但中文输入错误往往与拼音有关。

例如：

``` text
擎 → qing
晴 → qing
```

可以扩展：

``` text
完全相同     → 0
同音字       → 0.2
拼音相近     → 0.4
形近字       → 0.6
普通替换     → 1.0
```

DP：

``` cpp
dp[i][j] = std::min({
    dp[i - 1][j] + deleteCost,
    dp[i][j - 1] + insertCost,
    dp[i - 1][j - 1] + replaceCost(a[i - 1], b[j - 1])
});
```

推荐 Query Correction Pipeline：

``` text
                   用户 Query
                       │
                       ▼
                UTF-8 Decode
                       │
                       ▼
                  中文分词
                       │
                       ▼
                 拼写错误检测
                       │
                       ▼
               Candidate Recall
                       │
              ┌────────┴────────┐
              ▼                 ▼
           拼音索引           字符索引
              │                 │
              └────────┬────────┘
                       ▼
             Weighted Edit Distance
                       │
                       ▼
                Candidate Ranking
                       │
                       ▼
                  Query Rewrite
                       │
                       ▼
               BM25 / Hybrid Search
```

------------------------------------------------------------------------

## 14. 倒排索引与 BM25

本节描述文档检索，不描述当前字符到词条的推荐索引。现有 `en_dict.dat` / `cn_dict.dat` 只含全局词频，缺少文档频率、文档长度和文档归属，无法仅凭这些文件计算文档 BM25。

传统全文检索仍然是现代搜索和 RAG 的重要基础。

``` text
Document
   ↓
Tokenizer
   ↓
Inverted Index
   ↓
BM25
   ↓
Top-K
```

倒排索引：

``` text
epoll   → doc1, doc5, doc8
reactor → doc2, doc5
linux   → doc1, doc2, doc9
```

BM25 通常需要：

-   Term Frequency
-   Document Frequency
-   Document Length
-   Average Document Length
-   IDF
-   BM25 Score

先完成 V1 关键词推荐，再进入 V2 文档建库与检索。按课程资料实现 TF-IDF / 余弦相似度基线，再用同一数据集比较 BM25；完成词项到文档的倒排索引和效果评测后，才讨论向量检索。

------------------------------------------------------------------------

## 15. RAG 没有替代传统搜索

第 15 至 21 节均为长期概念说明，当前没有对应实现，也不属于本轮验收。协议 / SDK 的具体实现需在真正接入时检查当时的官方规范。

关键认识：

> **RAG 中的 R 就是 Retrieval。传统搜索没有被 RAG 淘汰，反而可以成为 RAG
> 的底层检索能力。**

传统 Search：

``` text
Query
  ↓
Query Understanding
  ↓
Inverted Index
  ↓
BM25
  ↓
Ranking
  ↓
Documents
```

RAG：

``` text
Question
   ↓
Retrieval
   ↓
Relevant Documents / Chunks
   ↓
LLM
   ↓
Answer
```

整体关系：

``` text
             Search / Retrieval
                    │
        ┌───────────┼───────────┐
        ↓           ↓           ↓
      BM25       Vector      Knowledge
     Search       Search       Graph
        │           │           │
        └───────────┼───────────┘
                    ↓
                 Rerank
                    │
             ┌──────┴──────┐
             ↓             ↓
        Search UI          RAG
             │             │
        Documents          LLM
                           │
                         Answer
```

------------------------------------------------------------------------

## 16. 为什么不能简单用 Vector Search 替代 BM25

Vector Search 对语义问题有优势，例如：

``` text
Linux 中有什么机制可以同时管理大量网络连接？
```

即使文档写的是：

``` text
epoll 是 Linux 下高效的 IO 多路复用机制
```

向量检索仍可能召回。

但对于：

``` text
std::bad_alloc
0x80070005
epoll_wait
函数名
错误码
型号
专有名词
```

关键词 / 倒排索引通常非常重要。

因此更推荐：

> **BM25 + Vector Search = Hybrid Retrieval**

而不是"Vector DB 替代搜索引擎"。

------------------------------------------------------------------------

## 17. Hybrid Retrieval

推荐：

``` text
                 Query
                   │
          ┌────────┴────────┐
          ↓                 ↓
       BM25              Embedding
          │                 │
   Inverted Index       Vector Index
          │                 │
        Top-K             Top-K
          └────────┬────────┘
                   ↓
             Fusion / RRF
                   ↓
                Rerank
                   ↓
                 Top-K
```

Top-K 可以同时提供：

``` text
Top-K
  │
  ├────────────→ Search UI
  │
  └────────────→ RAG → LLM → Answer
```

------------------------------------------------------------------------

## 18. MCP 是什么

MCP = Model Context Protocol。

核心理解：

> **MCP 不是因为 LLM
> 原来不能调用工具，而是为了把工具和数据以标准化、可发现、可复用的方式提供给不同
> AI Client。**

类比：

``` text
HTTP       → 协议
Web Server → 实现 HTTP 的服务

MCP        → 协议
MCP Server → 实现 MCP、向 Client 提供能力的服务
```

以前：

``` text
Agent A → 自定义 API
Agent B → 另一套 API
Agent C → 又一套 API
```

MCP：

``` text
Agent A ─┐
Agent B ─┼→ MCP → Tool Server
Agent C ─┘
```

MCP 本身不会使搜索算法更强，它主要解决 AI 工具接入的标准化问题。

------------------------------------------------------------------------

## 19. MCP、MCP Server、Client、Tool 的关系

  名称          含义
  ------------- -----------------------------
  MCP           协议 / 标准
  MCP Client    调用 MCP Server 的客户端
  MCP Server    按 MCP 协议提供能力的程序
  Tool          MCP Server 暴露的可调用功能
  Resource      MCP Server 提供的可读取资源
  Agent / LLM   使用这些能力完成任务的一方

例如：

``` text
Agent
  │
MCP Client
  │
  │ MCP Protocol
  ▼
MCP Server
  │
  ├── search()
  ├── semantic_search()
  ├── get_document()
  └── get_related_docs()
  │
  ▼
Search Service
```

------------------------------------------------------------------------

## 20. MCP Server 在本项目中的正确架构

不要：

``` text
MCP Server
   ↓
所有搜索代码
   ↓
BM25
   ↓
Redis
   ↓
MySQL
```

推荐：

``` text
Agent
 ↓
MCP Client
 ↓
MCP Server
 ↓
HTTP / RPC / Internal API
 ↓
Search Service
 ↓
Search Engine
```

因此：

``` text
Web Client ──HTTP────┐
                     │
Agent ──MCP Server───┼──→ SearchService → SearchEngine
                     │
RPC Client ──────────┘
```

核心原则：

> **MCP 是接入层，SearchEngine 是业务 / 基础设施层。**

------------------------------------------------------------------------

## 21. MCP Server 能否单独作为项目

可以。

例如：

``` text
Agent
  │
MCP Client
  │
MCP Server
  │
  ├── search_code()
  ├── read_source()
  ├── find_symbol()
  └── find_references()
  │
C++ Code Search Engine
```

但如果只是：

``` text
MCP SDK
 ↓
3 个 Tool
 ↓
调用第三方 API
```

项目容易显得比较薄。

对于 C++ 后端方向，更好的组合是：

> **真正实现 Search Service，然后通过 MCP 暴露搜索能力。**

------------------------------------------------------------------------

## 22. 数据层

当前只使用本地语料、停用词及词典 / 字符索引文件。配置出现缓存键不等于 Redis 已接入，资料目录存在压缩包不等于依赖已经构建。

首版由 `DictionaryRepository` 在启动时加载两套词库，一次校验后发布只读快照。没有需要关系查询的元信息时，不引入 MySQL；没有重复查询基准时，不把 Redis 设为启动依赖。下方是文档检索成熟后的可选分层。

长期建议：

``` text
                  Search Service
                       │
        ┌──────────────┼──────────────┐
        ↓              ↓              ↓
     Index          Metadata        Cache
        │              │              │
        ↓              ↓              ↓
   Disk/File          MySQL           Redis
```

### Index

保存：

-   Inverted Index
-   Term Dictionary
-   Document Index
-   Vector Index（后续）

### MySQL

保存：

``` text
document_id
title
author
path
category
create_time
metadata
```

### Redis

用于：

-   热门查询缓存
-   搜索结果缓存
-   文档缓存
-   统计 / Session 等辅助数据

如果 Redis Client 是同步阻塞式，不应在 IO EventLoop 中直接调用。

------------------------------------------------------------------------

## 23. 索引并发

搜索引擎通常是：

> **读多写少。**

当前标准是 C++11；原讨论中的 `std::shared_mutex` 是 C++17 接口，`std::shared_lock` 是 C++14 接口，无模板实参的锁对象示例也不能直接用于本工程。没有升级语言标准的授权时，不照搬这些写法。

首版词库在启动前完整加载并校验，之后共享 `std::shared_ptr<const DictionaryRepository>`。查询期间不修改词条、索引或预解码字符，不需要为每次检索加锁。DP、候选集合和结果都放在请求局部；类名含 `const` 不代表其内部缓存可以无保护地写入。

若后续实现热更新，在后台构造完整新快照，通过短临界区交换指针，旧请求持有旧快照直至完成。C++11 兼容的概念示例：

```cpp
std::mutex snapshotMutex;
std::shared_ptr<const DictionaryRepository> currentSnapshot;

std::shared_ptr<const DictionaryRepository> acquireSnapshot()
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    return currentSnapshot;
}

void publishSnapshot(std::shared_ptr<const DictionaryRepository> next)
{
    std::lock_guard<std::mutex> lock(snapshotMutex);
    currentSnapshot.swap(next);
}
```

只在获取 / 发布指针时持锁，文件加载与推荐计算不持锁。发布前必须校验成功，发布后不得通过其他可变别名修改底层对象。原子访问共享指针也可使用 C++11 提供的对应自由函数，但不要误用较新标准的 `std::atomic<std::shared_ptr<T>>` 特化。

后续可以演进：

``` text
Immutable Main Index
        +
Incremental Index
        +
Background Merge
```

避免查询线程与索引更新线程长期争用同一把独占锁。

------------------------------------------------------------------------

## 24. 文档索引 Pipeline

文档检索离线流水线已经落地：`PageProcessor` 从 `docs/模块二/人民网语料/` 读取 XML，按 `content` 优先、`description` 回退提取正文，以 SimHash 汉明距离不超过 3 去重，然后生成连续文档编号、网页库和字节偏移库。网页库仍保存原始 HTML 字符串并由 tinyxml2 转义；建立索引时另外将标签和实体替换成分词边界，不改变网页库原文。

索引使用标题和清洗后的正文，保留汉字和小写 ASCII 字母，由 cppjieba Mix 分词并过滤中英文停用词。实现统计 TF / DF，使用 `TF * log2(N/DF)`，再按文档做 L2 归一化；版本目录中的 `inverted_index.dat` 按“词项 文档编号 权重...”保存。现有字符到词典词条的索引仍不能复用为网页倒排索引。

网页三份产物现在写入 `data/web_index/generations/<generation>/`，清单记录 SHA-256、数量、格式版本和建库输入。发布器校验完整目录后才原子替换 `CURRENT`。`WebIndexStore::loadCurrent()` 只读取一次版本指针，完整校验并返回只读 `WebIndexSnapshot`；`readDocument()` 按偏移从快照持有的网页文件描述符读取。正式 `SearchEngineServer` 启动时加载快照，核对当前 cppjieba 和停用词资源指纹；Echo 示例不参与该业务。

``` text
PDF / HTML / Markdown / TXT
             ↓
        Document Loader
             ↓
        Text Extraction
             ↓
           Cleaning
             ↓
         Tokenization
             ↓
       ┌─────┴──────┐
       ↓            ↓
   Inverted       Embedding
    Index            │
       │             ↓
       │         Vector Index
       │
       └──────┬──────┘
              ↓
          Index Storage
```

多格式 PDF / HTML / Markdown 解析与 Embedding 在课程网页闭环之后考虑。读取 PDF 设计资料不等于实现了 PDF 文档入库。

------------------------------------------------------------------------

## 25. 长期整体架构（不代表当前类图）

近期结构是“TLV 客户端 → 单 Reactor → Worker Pool → KeywordRecommender → Reactor 回传”。下面保留 HTTP / MCP / Hybrid 的目标架构，仅在相应业务和测量条件满足后逐步实现。

``` text
                         User
                          │
              ┌───────────┴───────────┐
              │                       │
          Web Client              LLM / Agent
              │                       │
            HTTP                  MCP Client
              │                       │
              │                   MCP Server
              │                       │
              └───────────┬───────────┘
                          ▼
                    Search Service
                          │
                  Search Worker Pool
                          │
                          ▼
                    Search Engine
                          │
          ┌───────────────┼────────────────┐
          ▼               ▼                ▼
   QueryProcessor    InvertedIndex    VectorIndex
          │               │                │
   SpellCorrector        BM25          Embedding
          │               │                │
          └───────────────┼────────────────┘
                          ▼
                    Hybrid Rank
                          │
                       Rerank
                          │
                        Top-K
                          │
              ┌───────────┴───────────┐
              ▼                       ▼
          Search UI                  RAG
                                      │
                                     LLM
                                      │
                                    Answer
```

底层服务运行：

``` text
Client
  ↓
Main Reactor
  ↓
IO Reactor
  ↓
HTTP Parser / Router
  ↓
SearchController
  ↓
Worker Pool
  ↓
SearchService
  ↓
SearchEngine
  ↓
queueInLoop()
  ↓
eventfd wakeup
  ↓
IO Reactor
  ↓
HttpResponse
  ↓
Socket
```

------------------------------------------------------------------------

## 26. 推荐目录结构

基于已有目录增量扩展，不把现有网络代码搬成另一套框架。`doc/` 放本项目新文档，`docs/` 保留课程资料与语料，二者不是同一目录。

```text
searchengine/
├── CMakeLists.txt
├── conf/myconf.conf                   # 当前配置，尚无解析器
├── data/                              # 当前中英文产物
├── doc/
│   ├── search-engine-context.md        # 本文
│   └── 搜索引擎项目迭代方案.md
├── docs/                              # 当前课程资料和语料
├── src/
│   ├── offlineprocess/
│   │   ├── corpus/                    # 当前语料、词典和字符索引模块
│   │   ├── webpage/                   # 当前网页库和偏移库模块
│   │   ├── DirectoryScanner.*         # 当前共享目录扫描
│   │   └── Utf8Utils.*                # 当前共享 UTF-8 工具
│   ├── reactor/                       # 当前网络、线程池、测试与 Echo 示例
│   ├── common/                        # 拟新增：迁入公共 Utf8Utils
│   ├── recommendation/                # 拟新增：加载、归一化、距离、推荐
│   ├── protocol/                      # 拟新增：帧与业务 JSON
│   └── online/                        # 拟新增：配置、服务入口与网络客户端
├── tests/                             # 拟新增测试与 fixtures
├── build/                             # 构建输出
└── bin/                               # 当前 CMake 可执行文件输出
```

命令行推荐入口拟放在 `src/recommendation/RecommendMain.cpp`，配置和网络入口拟放在 `src/online/`。目标拆分与具体文件以迭代方案第 8 节为准。图中新增目录尚不存在，不提前创建 HTTP、向量、MCP、RAG 等空壳。

------------------------------------------------------------------------

## 27. 推荐给 Codex 的工作原则

Codex 读取本文档后应遵循：

1.  **先检查仓库，后修改代码。**
2.  不要假定本文档描述的模块已经存在。
3.  优先复用现有 Reactor / 网络框架，而不是重新写一套。
4.  保持网络层、协议层、Service 层、SearchEngine 层解耦。
5.  EventLoop 只做 IO 和轻量调度，不执行耗时检索。
6.  搜索任务进入 Worker Pool。
7.  Worker 完成后通过 `queueInLoop()` 等机制回到所属 EventLoop。
8.  现有 eventfd / pending functors 已实现，保留并验证，不重复建设。
9.  V1 优先完成在线关键词推荐；全文检索与 AI 扩展属于后续版本。
10. 中文字符串处理必须考虑 UTF-8 / Unicode，不能直接按 byte
    计算编辑距离。
11. Query Correction 应先 Candidate Recall，再计算编辑距离；明确字符索引的召回限制。
12. SearchEngine 不应该依赖 MCP。
13. MCP Server 只作为 Agent 接入层。
14. Vector Search 是 BM25 的补充，不是简单替代。
15. RAG 使用 Search / Retrieval 的结果，不应该推翻底层检索系统。
16. 每次进行较大架构修改前，先给出修改计划、涉及文件和原因。
17. 避免为了"架构完整"一次创建大量空模块。
18. 每个版本都应保持项目可编译、可运行、可测试；区分已执行测试与待实现验收。
19. 保持 C++11 兼容，除非用户明确同意升级标准。
20. Reactor 提交任务必须非阻塞；连接状态、缓冲和请求登记仅在所属 EventLoop 修改。
21. 保留独立中英文编号，不直接串联两个索引；数据加载失败应报错而非跳过行。
22. 不能把线程池排空当成响应排空，也不能通过持有连接指针假定连接有效。

------------------------------------------------------------------------

## 28. 推荐 Codex 首次接手时执行的任务

将本文档放到：

``` text
doc/search-engine-context.md
```

然后可以对 Codex 输入：

``` text
请先完整阅读 doc/search-engine-context.md 与 doc/搜索引擎项目迭代方案.md，
然后检查当前仓库代码。

注意：
两份文档明确区分当前实现与计划；具体能力仍需以本次源码检查为准。

暂时不要修改代码。

请输出：

1. 当前仓库的实际目录和主要模块；
2. 当前 Reactor 已经实现了哪些能力；
3. EventLoop / TcpConnection / ThreadPool 的调用与所有权关系；
4. 当前是否已经支持 Multi-Reactor；
5. 是否已经实现 runInLoop / queueInLoop / eventfd 跨线程唤醒；
6. 中英文建库、Utf8Utils 与当前四份产物的真实格式；
7. 当前配置、行帧/TLV 消息边界、Echo 阻塞提交和半关闭的限制；
8. 与 V1 推荐闭环相比缺少哪些模块和验收；
9. 下一步最适合实现的一个模块是什么；
10. 给出一个小步迭代计划，不要一次性大规模重构。
```

------------------------------------------------------------------------

## 29. 项目的核心技术主线

按当前代码调整后的交付顺序如下；越靠后的节点越需要明确需求与效果验证，不要求一次全部实现：

``` text
C++11 / Linux + 单 Reactor / epoll / eventfd
     ↓
中英文离线词典 + 字符索引（已完成）
     ↓
词库加载 + Unicode 推荐算法 + CLI（已完成）
     ↓
TLV / JSON + 非阻塞提交 + 在线推荐服务（已完成）
     ↓
端到端验收 + 延迟基线 + 按需缓存
     ↓
XML 网页库 + 词项到文档的倒排索引
     ↓
TF-IDF / 余弦相似度基线 + BM25 对照
     ↓
按需 HTTP / Multi-Reactor / 存储升级
     ↓
Vector Search
     ↓
Hybrid Retrieval
     ↓
Rerank
     ↓
RAG
     ↓
MCP
     ↓
Agent
```

其中：

``` text
C++ / Reactor / Search Engine
```

是项目主体。

``` text
RAG / MCP / Agent
```

是建立在检索系统之上的现代扩展。

------------------------------------------------------------------------

## 30. 最终项目描述参考

当前可以准确描述为：

> 基于 C++11 / Linux 的搜索引擎基础工程，已实现中英文离线关键词建库、Unicode 编辑距离关键词推荐 CLI 与 TLV 在线服务、tinyxml2 网页提取、SimHash 文档去重、网页库与字节偏移库、归一化 TF-IDF 倒排索引，以及 TF-IDF 余弦在线网页查询；两类业务共用 epoll / eventfd 单 Reactor、非阻塞线程池提交、半关闭和排空停机机制。

只有最终完成对应模块并有性能证据后，才可以使用下面的目标描述。不能把它直接当作当前成果或简历中的已完成能力：

> **基于 C++ / Linux 实现面向 RAG 与 Agent 的高性能 Hybrid Search
> Engine。底层采用 Multi-Reactor + Worker Pool 构建并发搜索服务，通过
> epoll/eventfd 实现 IO 与检索任务解耦；检索层实现中文 Query
> Processing、拼写纠错、倒排索引、BM25、Top-K，并进一步融合向量检索与
> Reranking；上层通过 HTTP / MCP 将检索能力提供给 Web 客户端和 LLM
> Agent，并作为 RAG 的 Retrieval 基础设施。**

项目的价值不应依赖"MCP""Agent"等热门关键词，而应能够从代码层面展示：

-   网络框架设计
-   并发模型
-   C++ 工程能力
-   搜索算法
-   中文文本处理
-   索引结构
-   排序算法
-   缓存与存储
-   模块解耦
-   性能分析与优化
-   从传统 Search 到现代 AI Retrieval 的演进能力

------------------------------------------------------------------------

## 31. 一句话架构原则

> **Reactor 负责高并发运行，SearchEngine 负责检索，Hybrid Retrieval
> 提升召回，RAG 负责生成，MCP 负责标准化 Agent 接入。**

落实到当前阶段：网页分支已完成“XML → 去重网页库/偏移库 → TF-IDF 倒排索引 → generation 发布与校验加载 → 查询分词/候选交集/余弦排序 → TLV 响应”；在线推荐分支已完成“语料 → 四文件 generation 发布与校验加载 → TLV/JSON 请求 → 非阻塞推荐计算 → TLV 响应 → 半关闭/排空停机”。两类业务已有本机性能基线；下一步是标注查询集和长时负载测量。
