# QAIService

QAIService 是一个基于 C++17 和 Muduo 构建的 AI 私人管家服务平台。它把普通对话、私人知识检索、生活数据管理和联网证据查询整合到同一个多用户 Web 应用中，并通过明确的线程边界隔离数据库、模型、消息队列和公网请求等阻塞操作。

项目希望解决的不是单次模型调用，而是 AI 应用落地后的完整服务问题：用户身份与会话如何持久化、同一对话如何保证上下文顺序、回答如何可靠落库、私人资料如何参与检索、模型写入业务数据前如何获得用户确认，以及联网抓取如何控制隐私和 SSRF 风险。

## 核心功能

- 多用户注册、登录、退出与持久化 Session
- 普通对话与私人管家双模式、多会话上下文和任务进度恢复
- TXT、Markdown、PDF、DOCX 文档上传、解析、检索和证据问答
- 日历、待办、专注、打卡、账户、预算、收支、体重和睡眠管理
- 模型生成白名单写入提案，用户确认后才修改业务数据
- 可选联网检索、查询脱敏、单次授权、来源保留和安全网页抓取
- RabbitMQ 异步传递聊天事件，MySQL 事务提交后再确认消费

## 系统结构

```text
Browser
  │
  ▼
Muduo TCP / Reactor
  └─ HTTP Codec → Middleware → Router
       ├─ DatabaseWorker ───────────────→ MySQL
       ├─ Worker Pool ─────────────────→ Model API
       │      ├─ Retrieval Service ────→ BGE Reranker
       │      └─ Web Search / Fetcher ─→ Public Web
       └─ Publisher → RabbitMQ → Consumer → MySQL
```

EventLoop 只负责网络事件、HTTP 解析、轻量路由和响应发送。MySQL、模型调用、RabbitMQ 发布与消费、语义重排和公网访问运行在独立执行位置，避免慢任务阻塞网络线程。

## 技术栈

- C++17、Muduo、CMake、Ninja
- MySQL 8.4、RabbitMQ 4.1
- libcurl、libxml2、Poppler、libzip
- libsodium、spdlog、nlohmann/json
- Python 3.11、FastAPI、BGE Reranker
- Docker、Docker Compose

## 快速启动

### 1. 环境要求

- Docker Desktop 或 OrbStack
- Docker Compose v2
- 建议至少预留 4 GB 可用内存

### 2. 创建本地配置

```bash
cp .env.example .env
```

至少为下面三个变量设置不同的本地密码：

```dotenv
QAI_DB_PASSWORD=replace-with-a-database-password
QAI_DB_ROOT_PASSWORD=replace-with-a-root-password
QAI_RABBITMQ_PASSWORD=replace-with-a-rabbitmq-password
```

`.env` 已被 Git 忽略，不要提交真实密码或 API Key。

### 3. 启动服务

```bash
./run.sh
```

该命令会构建服务镜像，启动 MySQL 和 RabbitMQ，执行数据库迁移，并在依赖健康后启动 QAIService。

服务启动后访问：

```text
http://127.0.0.1:8080/
```

检查容器状态：

```bash
./status.sh
```

检查服务健康状态：

```bash
curl http://127.0.0.1:8080/health
```

正常响应：

```json
{"status":"ok"}
```

## 模型配置

默认使用 Mock 提供方，无需 API Key 即可启动和体验完整请求链路。接入兼容 OpenAI Chat Completions 协议的模型服务时，在 `.env` 中设置：

```dotenv
QAI_CHAT_PROVIDER=openai-compatible
QAI_MODEL_BASE_URL=https://your-provider.example/v1
QAI_MODEL_NAME=your-model-name
QAI_MODEL_API_KEY=your-api-key
```

修改配置后重新创建服务容器：

```bash
docker compose up --detach --build --force-recreate server
```

## 可选语义重排

私人知识检索默认可以使用词项召回。启用 BGE 语义重排时，在 `.env` 中设置：

```dotenv
QAI_RERANK_ENABLED=true
QAI_TOKEN_ENABLED=true
```

然后启动检索服务和主服务：

```bash
docker compose --profile rag up --detach --build retrieval server
```

首次启动需要下载并加载模型，耗时和磁盘占用取决于模型与网络环境。模型文件保存在 Docker Volume 中，重建容器时可以复用。

## 可选联网检索

联网检索默认关闭。配置百度千帆搜索后可以启用：

```dotenv
QAI_WEB_SEARCH_ENABLED=true
QAI_WEB_SEARCH_PROVIDER=baidu
QAI_BAIDU_SEARCH_API_KEY=your-api-key
```

用户仍需在聊天输入区为单条消息允许联网；检测到敏感查询时，系统会在实际出站前要求单次确认。

## 停止服务

停止容器但保留容器和数据：

```bash
./run.sh stop
```

停止并删除容器，但保留数据库、消息队列、上传文件和模型数据卷：

```bash
./run.sh down
```

如需同时删除所有持久化数据卷，可以执行：

```bash
docker compose --profile rag down --volumes
```

最后一条命令会永久删除本项目的数据库、消息队列、上传文件和模型缓存，请仅在确认不需要这些数据时使用。
