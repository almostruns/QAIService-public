cmake_minimum_required(VERSION 3.25)

project(QAIService VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_path(POPPLER_CPP_INCLUDE_DIR poppler-document.h PATH_SUFFIXES poppler/cpp REQUIRED)
find_library(POPPLER_CPP_LIBRARY poppler-cpp REQUIRED)
find_path(LIBXML2_INCLUDE_DIR libxml/parser.h PATH_SUFFIXES libxml2 REQUIRED)
find_library(LIBXML2_LIBRARY xml2 REQUIRED)
find_path(LIBZIP_INCLUDE_DIR zip.h REQUIRED)
find_library(LIBZIP_LIBRARY zip REQUIRED)

find_path(MUDUO_INCLUDE_DIR muduo/net/TcpServer.h REQUIRED)
find_library(MUDUO_NET_LIBRARY muduo_net REQUIRED)
find_library(MUDUO_BASE_LIBRARY muduo_base REQUIRED)
find_path(MARIADB_INCLUDE_DIR mysql.h PATH_SUFFIXES mariadb REQUIRED)
find_library(MARIADB_LIBRARY mariadb REQUIRED)
find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
find_library(SODIUM_LIBRARY sodium REQUIRED)
find_path(RABBITMQ_INCLUDE_DIR amqp.h REQUIRED)
find_library(RABBITMQ_LIBRARY rabbitmq REQUIRED)
find_package(spdlog REQUIRED)

add_library(qaiservice_logging tools/logging/log.cpp)
target_compile_features(qaiservice_logging PUBLIC cxx_std_17)
target_include_directories(qaiservice_logging PUBLIC tools)
target_link_libraries(qaiservice_logging PUBLIC spdlog::spdlog Threads::Threads)

add_library(
  qaiservice_web_search
  src/web_search/baidu_search_provider.cpp
  src/web_search/mock_search_provider.cpp
  src/web_search/query_privacy_guard.cpp
  src/web_search/safe_page_fetcher.cpp
  src/web_search/search_consent_store.cpp
  src/web_search/search_result_selector.cpp
  src/web_search/url_safety.cpp
  src/web_search/web_answer_envelope.cpp
  src/web_search/web_cache.cpp
  src/web_search/web_evidence_service.cpp
  src/web_search/web_page_extractor.cpp
  src/web_search/web_search_router.cpp
  src/web_search/web_search_coordinator.cpp
)
target_compile_features(qaiservice_web_search PUBLIC cxx_std_17)
target_include_directories(qaiservice_web_search PUBLIC src)
target_include_directories(qaiservice_web_search PRIVATE ${LIBXML2_INCLUDE_DIR})
target_link_libraries(
  qaiservice_web_search
  PRIVATE CURL::libcurl ${LIBXML2_LIBRARY} ${SODIUM_LIBRARY} qaiservice_logging
)

add_executable(qaiservice src/main.cpp)
target_compile_features(qaiservice PRIVATE cxx_std_17)

add_library(
  qaiservice_chat
  src/chat/chat_model_config.cpp
  src/chat/chat_progress_store.cpp
  src/chat/chat_routes.cpp
  src/chat/chat_service.cpp
  src/chat/conversation_title.cpp
  src/chat/mock_chat_model_provider.cpp
  src/chat/openai_compatible_chat_provider.cpp
)
target_compile_features(qaiservice_chat PUBLIC cxx_std_17)
target_include_directories(qaiservice_chat PUBLIC src PRIVATE ${MUDUO_INCLUDE_DIR})

add_library(
  qaiservice_persistence
  src/persistence/chat_event_consumer.cpp
  src/persistence/chat_event_publisher.cpp
  src/persistence/chat_conversation_repository.cpp
  src/persistence/chat_message_event.cpp
  src/persistence/chat_message_repository.cpp
  src/persistence/rabbitmq_config.cpp
  src/persistence/rabbitmq_connection.cpp
)
target_compile_features(qaiservice_persistence PUBLIC cxx_std_17)
target_include_directories(
  qaiservice_persistence
  PUBLIC src
  PRIVATE ${MARIADB_INCLUDE_DIR} ${RABBITMQ_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR}
)
target_link_libraries(
  qaiservice_persistence
  PUBLIC qaiservice_chat qaiservice_db
  PRIVATE ${MARIADB_LIBRARY} ${RABBITMQ_LIBRARY} ${SODIUM_LIBRARY} qaiservice_logging
)

add_library(
  qaiservice_http
  src/http/health.cpp
  src/http/http_response_writer.cpp
  src/http/loop_bound_response_writer.cpp
  src/http/middleware.cpp
  src/http/router.cpp
  src/http/web_app.cpp
)
target_compile_features(qaiservice_http PUBLIC cxx_std_17)
target_include_directories(qaiservice_http PUBLIC src)
target_link_libraries(qaiservice_http PRIVATE qaiservice_logging)

add_library(
  qaiservice_db
  src/db/database_config.cpp
  src/db/database_worker.cpp
  src/db/mysql_connection.cpp
  src/db/mysql_statement.cpp
)
target_compile_features(qaiservice_db PUBLIC cxx_std_17)
target_include_directories(
  qaiservice_db
  PUBLIC src
  PRIVATE ${MARIADB_INCLUDE_DIR}
)
target_link_libraries(qaiservice_db PUBLIC Threads::Threads
                      PRIVATE ${MARIADB_LIBRARY} qaiservice_logging)

add_library(
  qaiservice_users
  src/users/auth_middleware.cpp
  src/users/auth_routes.cpp
  src/users/password_hasher.cpp
  src/users/session_repository.cpp
  src/users/session_store.cpp
  src/users/user_repository.cpp
)

add_library(
  qaiservice_knowledge
  src/knowledge/document_repository.cpp
  src/knowledge/document_extractor.cpp
  src/knowledge/file_storage.cpp
  src/knowledge/knowledge_answer_service.cpp
  src/knowledge/knowledge_routes.cpp
  src/knowledge/rerank_client.cpp
  src/knowledge/retriever.cpp
  src/knowledge/token_client.cpp
)
target_compile_features(qaiservice_knowledge PUBLIC cxx_std_17)
target_include_directories(
  qaiservice_knowledge
  PUBLIC src
  PRIVATE ${MARIADB_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR} ${POPPLER_CPP_INCLUDE_DIR} ${LIBXML2_INCLUDE_DIR}
          ${LIBZIP_INCLUDE_DIR}
)
target_link_libraries(
  qaiservice_knowledge
  PUBLIC qaiservice_db qaiservice_http qaiservice_users
  PRIVATE qaiservice_logging
  PRIVATE CURL::libcurl ${MARIADB_LIBRARY} ${SODIUM_LIBRARY} ${POPPLER_CPP_LIBRARY} ${LIBXML2_LIBRARY}
          ${LIBZIP_LIBRARY}
)

target_compile_features(qaiservice_users PUBLIC cxx_std_17)
target_include_directories(
  qaiservice_users
  PUBLIC src
  PRIVATE ${MARIADB_INCLUDE_DIR} ${SODIUM_INCLUDE_DIR}
)
target_link_libraries(
  qaiservice_users
  PUBLIC qaiservice_db qaiservice_http
  PRIVATE ${MARIADB_LIBRARY} ${SODIUM_LIBRARY} qaiservice_logging
)

add_library(
  qaiservice_muduo
  src/chat/chat_conversation_routes.cpp
  src/http/http_codec.cpp
  src/http/http_connection_handler.cpp
  src/http/qai_http_server.cpp
)

add_library(qaiservice_life src/life/activity_insights.cpp src/life/financial_insights.cpp
                            src/life/life_record_repository.cpp src/life/life_routes.cpp
                            src/life/life_validation.cpp)
target_compile_features(qaiservice_life PUBLIC cxx_std_17)
target_include_directories(qaiservice_life PUBLIC src
                           PRIVATE ${MARIADB_INCLUDE_DIR})
target_link_libraries(qaiservice_life PUBLIC qaiservice_db qaiservice_http qaiservice_users
                      PRIVATE ${MARIADB_LIBRARY} qaiservice_logging)

add_library(qaiservice_assistant src/assistant/assistant_routes.cpp src/assistant/mock_intent_parser.cpp
                                 src/assistant/private_assistant.cpp src/assistant/proposal_store.cpp)
target_compile_features(qaiservice_assistant PUBLIC cxx_std_17)
target_include_directories(qaiservice_assistant PUBLIC src PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(qaiservice_assistant PUBLIC qaiservice_db qaiservice_http qaiservice_knowledge qaiservice_life
                                                   qaiservice_users
                      PRIVATE qaiservice_logging ${SODIUM_LIBRARY})
target_compile_features(qaiservice_muduo PUBLIC cxx_std_17)
target_include_directories(
  qaiservice_muduo
  PUBLIC src ${MUDUO_INCLUDE_DIR}
)
target_link_libraries(
  qaiservice_muduo
  PUBLIC
    qaiservice_chat
    qaiservice_assistant
    qaiservice_http
    qaiservice_knowledge
    qaiservice_life
    qaiservice_persistence
    qaiservice_users
    qaiservice_web_search
    qaiservice_logging
    ${MUDUO_NET_LIBRARY}
    ${MUDUO_BASE_LIBRARY}
    Threads::Threads
)
target_link_libraries(qaiservice PRIVATE qaiservice_muduo qaiservice_logging)

target_link_libraries(qaiservice_chat PUBLIC qaiservice_http qaiservice_users
                      PRIVATE CURL::libcurl qaiservice_logging)
