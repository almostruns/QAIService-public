#pragma once

#include "db/mysql_connection.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace qaiservice::db {

class DatabaseWorker {
 public:
  using Task = std::function<void(MySqlConnection&)>;

  explicit DatabaseWorker(DatabaseConfig config, std::size_t max_pending_tasks = 64);
  ~DatabaseWorker();

  DatabaseWorker(const DatabaseWorker&) = delete;
  DatabaseWorker& operator=(const DatabaseWorker&) = delete;

  [[nodiscard]] bool submit(Task task);
  void stop();

 private:
  void run(DatabaseConfig config);

  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Task> tasks_;
  std::size_t max_pending_tasks_;
  bool ready_{false};
  bool stopping_{false};
  std::exception_ptr startup_error_;
  std::thread thread_;
};

}  // namespace qaiservice::db
