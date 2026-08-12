#include "http/loop_bound_response_writer.h"

#include <utility>

namespace qaiservice::http {

// EventLoop 绑定 Writer
ResponseWriter makeLoopBoundResponseWriter(LoopScheduler scheduler,
                                           ConnectionCheck connection_check,
                                           ResponseSink response_sink)
{
  ResponseWriter::SendFunction send_function =
      [scheduler = std::move(scheduler), connection_check = std::move(connection_check),
       response_sink = std::move(response_sink)](Response response) {
        LoopTask loop_task = [connection_check, response_sink, response = std::move(response)]() mutable {
          if (!connection_check()) {
            return;
          }

          response_sink(std::move(response));
        };
        scheduler(std::move(loop_task));
      };

  return ResponseWriter(std::move(send_function));
}

}  // namespace qaiservice::http
