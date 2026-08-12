#include "db/database_worker.h"

#include "logging/log.h"
#include <mysql.h>

#include <stdexcept>
#include <utility>

namespace qaiservice::db {
namespace {

class MySqlThreadContext {
 public:
  MySqlThreadContext()
  {
    if (mysql_thread_init() != 0) {
      throw DatabaseError("cannot initialize MySQL worker thread");
    }
  }

  ~MySqlThreadContext()
  {
    mysql_thread_end();
  }
};

}  // namespace

DatabaseWorker::DatabaseWorker(DatabaseConfig config, std::size_t max_pending_tasks)
    : max_pending_tasks_(max_pending_tasks)
{
  if (max_pending_tasks_ == 0) {
    throw std::invalid_argument("database worker capacity must be positive");
  }

  thread_ = std::thread([this, config = std::move(config)]() mutable {
    run(std::move(config));
  });

  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] {
    return ready_ || startup_error_ != nullptr;
  });

  if (startup_error_ != nullptr) {
    lock.unlock();
    thread_.join();
    std::rethrow_exception(startup_error_);
  }
}

DatabaseWorker::~DatabaseWorker()
{
  stop();
}

void DatabaseWorker::stop()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  condition_.notify_one();

  if (thread_.joinable()) {
    thread_.join();
  }
}

bool DatabaseWorker::submit(Task task)
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= max_pending_tasks_) {
      return false;
    }
    tasks_.push_back(std::move(task));
  }
  condition_.notify_one();
  return true;
}

void DatabaseWorker::run(DatabaseConfig config)
{
  try {
    MySqlThreadContext thread_context;
    MySqlConnection connection(config);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ready_ = true;
    }
    condition_.notify_one();

    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
          return stopping_ || !tasks_.empty();
        });

        if (stopping_ && tasks_.empty()) {
          break;
        }

        task = std::move(tasks_.front());
        tasks_.pop_front();
      }

      try {
        task(connection);
      } catch (const std::exception& error) {
        // 任务负责把错误交给自己的调用方；异常不能终止数据库线程。
        QAI_LOG(err, "db") << "database_task_exception error=" << error.what();
      }
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    startup_error_ = std::current_exception();
    condition_.notify_one();
  }
}

}  // namespace qaiservice::db
