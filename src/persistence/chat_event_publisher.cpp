#include "persistence/chat_event_publisher.h"

#include "logging/log.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace qaiservice::persistence {

struct ChatEventPublisher::State {
  State(std::unique_ptr<ChatEventTransport> owned_transport, std::size_t queue_capacity,
        std::size_t attempt_limit, std::chrono::milliseconds delay)
      : transport(std::move(owned_transport)), capacity(queue_capacity), max_attempts(attempt_limit),
        retry_delay(delay)
  {
  }

  std::unique_ptr<ChatEventTransport> transport;
  const std::size_t capacity;
  const std::size_t max_attempts;
  const std::chrono::milliseconds retry_delay;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<ChatMessageCreated> queue;
  std::size_t pending{0};
  std::size_t confirmed{0};
  std::size_t failed{0};
  bool stopping{false};
  std::thread thread;
};

ChatEventPublisher::ChatEventPublisher(std::unique_ptr<ChatEventTransport> transport, std::size_t capacity,
                                       std::size_t max_attempts, std::chrono::milliseconds retry_delay)
{
  if (transport == nullptr || capacity == 0 || max_attempts == 0 || retry_delay.count() < 0) {
    throw std::invalid_argument("invalid publisher configuration");
  }
  state_ = std::make_unique<State>(std::move(transport), capacity, max_attempts, retry_delay);
  state_->thread = std::thread([this] {
    run();
  });
}

ChatEventPublisher::~ChatEventPublisher()
{
  stop();
}

EnqueueStatus ChatEventPublisher::enqueue(std::vector<ChatMessageCreated> events)
{
  if (events.empty()) {
    QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "publisher_enqueue_rejected reason=empty_batch";
    return EnqueueStatus::kUnavailable;
  }

  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->stopping) {
      QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "publisher_enqueue_rejected reason=stopping";
      return EnqueueStatus::kUnavailable;
    }
    if (events.size() > state_->capacity - state_->pending) {
      QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "publisher_enqueue_rejected reason=queue_full"
                                                            << " event_count=" << events.size()
                                                            << " pending=" << state_->pending
                                                            << " capacity=" << state_->capacity;
      return EnqueueStatus::kBusy;
    }
    for (ChatMessageCreated& event : events) {
      state_->queue.push_back(std::move(event));
      ++state_->pending;
    }
  }
  state_->condition.notify_one();
  QAI_LOG(info, qaiservice::log::Module::kPersistence) << "publisher_events_queued event_count=" << events.size();
  return EnqueueStatus::kQueued;
}

PublisherSnapshot ChatEventPublisher::snapshot() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return {state_->pending, state_->confirmed, state_->failed};
}

void ChatEventPublisher::stop()
{
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->stopping = true;
  }
  state_->condition.notify_one();
  if (state_->thread.joinable()) {
    state_->thread.join();
  }
}

void ChatEventPublisher::run()
{
  while (true) {
    ChatMessageCreated event;
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      state_->condition.wait(lock, [this] {
        return state_->stopping || !state_->queue.empty();
      });
      if (state_->stopping && state_->queue.empty()) {
        return;
      }
      event = std::move(state_->queue.front());
      state_->queue.pop_front();
    }

    TransportPublishResult result = TransportPublishResult::kRetryableFailure;
    for (std::size_t attempt = 0; attempt < state_->max_attempts; ++attempt) {
      try {
        result = state_->transport->publish(event);
      } catch (const std::exception& error) {
        QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "publisher_attempt_failed event_id=" << event.event_id
                                                              << " attempt=" << attempt + 1
                                                              << " error=" << error.what();
        result = TransportPublishResult::kRetryableFailure;
      } catch (...) {
        QAI_LOG(warn, qaiservice::log::Module::kPersistence) << "publisher_attempt_failed event_id=" << event.event_id
                                                              << " attempt=" << attempt + 1
                                                              << " error=unknown";
        result = TransportPublishResult::kRetryableFailure;
      }
      if (result != TransportPublishResult::kRetryableFailure) {
        break;
      }
      if (attempt + 1 < state_->max_attempts && state_->retry_delay.count() > 0) {
        std::this_thread::sleep_for(state_->retry_delay);
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      --state_->pending;
      if (result == TransportPublishResult::kConfirmed) {
        ++state_->confirmed;
        QAI_LOG(info, qaiservice::log::Module::kPersistence) << "publisher_event_confirmed event_id=" << event.event_id
                                                              << " user_id=" << event.user_id
                                                              << " conversation_id=" << event.conversation_id
                                                              << " sequence=" << event.sequence;
      } else {
        ++state_->failed;
        QAI_LOG(err, qaiservice::log::Module::kPersistence) << "publisher_event_failed event_id=" << event.event_id
                                                             << " user_id=" << event.user_id
                                                             << " conversation_id=" << event.conversation_id
                                                             << " sequence=" << event.sequence;
      }
    }
  }
}

}  // namespace qaiservice::persistence
