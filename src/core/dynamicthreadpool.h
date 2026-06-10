#ifndef DYNAMICTHREADPOOL_H
#define DYNAMICTHREADPOOL_H

#include <iostream>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <thread>
#include <atomic>
#include <stdexcept>
#include <chrono>
#include <memory>
#include <algorithm>
#include <unordered_set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

// C++17: prefer std::apply from <tuple>, already included above

class DynamicThreadPool {
public:
    DynamicThreadPool(std::size_t coreThreads, std::size_t maxThreads,
                      std::size_t idleMs, std::size_t maxQueueSize,
                      std::size_t enqueueWaitMs = 50)
        : core_(coreThreads), max_(maxThreads), idleMs_(idleMs),
          maxQueue_(maxQueueSize), enqueueWaitMs_(enqueueWaitMs),
          activeWorkers_(0), stopped_(false)
    {
        if (core_ == 0 || max_ == 0 || core_ > max_) {
            throw std::invalid_argument("Invalid thread count parameters");
        }

        for (std::size_t i = 0; i < core_; ++i) {
            addWorker(true);
        }

        managerThread_ = std::thread(&DynamicThreadPool::manager, this);
    }

    ~DynamicThreadPool() {
        shutdown();
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>
    {
        using Ret = typename std::result_of<F(Args...)>::type;

        auto f_ptr    = std::make_shared<typename std::decay<F>::type>(std::forward<F>(f));
        auto args_tup = std::make_shared<std::tuple<typename std::decay<Args>::type...>>(
            std::forward<Args>(args)...);

        auto task = std::make_shared<std::packaged_task<Ret()>>(
            [f_ptr, args_tup]() -> Ret {
                if constexpr (std::is_void_v<Ret>) {
                    std::apply(std::move(*f_ptr), std::move(*args_tup));
                } else {
                    return std::apply(std::move(*f_ptr), std::move(*args_tup));
                }
            });

        std::future<Ret> fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(mtx_);

            bool enqueued = cvQueue_.wait_for(
                lock, std::chrono::milliseconds(enqueueWaitMs_),
                [this] { return stopped_ || tasks_.size() < maxQueue_; });

            if (stopped_) {
                throw std::runtime_error("Thread pool is stopped");
            }

            if (!enqueued) {
                throw std::runtime_error("Task queue full, enqueue timeout");
            }

            tasks_.emplace([task]() { (*task)(); });

            // 队列积压 > 60% 时尝试扩容
            if (tasks_.size() > maxQueue_ * 0.6 && getTotalThreads() < max_) {
                addWorker(false);
            }
        }

        cvWorkers_.notify_one();
        return fut;
    }

    std::size_t pendingTasks() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return tasks_.size();
    }

    std::size_t activeThreads() const {
        return activeWorkers_.load(std::memory_order_acquire);
    }

    std::size_t totalThreads() const {
        std::lock_guard<std::mutex> lock(workersMtx_);
        return workers_.size();
    }

    double utilization() const {
        std::size_t total = totalThreads();
        return total == 0 ? 0.0
                          : static_cast<double>(activeThreads()) / total;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopped_) return;
            stopped_ = true;
        }

        cvWorkers_.notify_all();
        cvQueue_.notify_all();

        if (managerThread_.joinable()) {
            managerThread_.join();
        }

        std::vector<std::thread> threadsToJoin;
        {
            std::lock_guard<std::mutex> lock(workersMtx_);
            threadsToJoin.swap(workers_);
        }

        for (auto& t : threadsToJoin) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    DynamicThreadPool(const DynamicThreadPool&) = delete;
    DynamicThreadPool& operator=(const DynamicThreadPool&) = delete;

private:
    void addWorker(bool isCore) {
        std::lock_guard<std::mutex> lock(workersMtx_);
        if (workers_.size() >= max_ || stopped_) return;

        workers_.emplace_back([this, isCore]() {
            workerLoop(isCore);
        });
    }

    void workerLoop(bool isCore) {
        auto selfId = std::this_thread::get_id();

        while (!stopped_) {
            std::unique_lock<std::mutex> lock(mtx_);

            if (isCore) {
                cvWorkers_.wait(lock, [this] {
                    return stopped_ || !tasks_.empty();
                });
            } else {
                if (!cvWorkers_.wait_for(lock, std::chrono::milliseconds(idleMs_),
                                         [this] {
                                             return stopped_ || !tasks_.empty();
                                         })) {
                    // 超时 → 检查是否应该缩容
                    if (shouldShrink()) {
                        break; // 退出循环, 标记为待清理
                    }
                    continue; // 继续等待
                }
            }

            if (stopped_ && tasks_.empty()) {
                break;
            }

            if (tasks_.empty()) {
                continue;
            }

            auto task = std::move(tasks_.front());
            tasks_.pop();
            lock.unlock();

            activeWorkers_.fetch_add(1, std::memory_order_release);

            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "Task execution error: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Task execution unknown error" << std::endl;
            }

            activeWorkers_.fetch_sub(1, std::memory_order_release);
        }

        markForRemoval(selfId);
    }

    void markForRemoval(std::thread::id tid) {
        std::lock_guard<std::mutex> lock(deadThreadsMtx_);
        deadThreads_.insert(tid);
    }

    void cleanupDeadThreads() {
        std::lock_guard<std::mutex> lockW(workersMtx_);
        std::lock_guard<std::mutex> lockD(deadThreadsMtx_);

        auto it = workers_.begin();
        while (it != workers_.end()) {
            if (deadThreads_.count(it->get_id())) {
                if (it->joinable()) {
                    it->join();
                }
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
        deadThreads_.clear();
    }

    bool shouldShrink() {
        std::lock_guard<std::mutex> lock(workersMtx_);
        return workers_.size() > core_ &&
               activeWorkers_.load(std::memory_order_acquire) < workers_.size() / 2;
    }

    std::size_t getTotalThreads() const {
        std::lock_guard<std::mutex> lock(workersMtx_);
        return workers_.size();
    }

    void manager() {
        while (!stopped_) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (stopped_) break;
            cleanupDeadThreads();
            if (shouldShrink()) {
                cvWorkers_.notify_all();
            }
        }
        cleanupDeadThreads();
    }

    // ─── 成员变量 ───
    const std::size_t core_;
    const std::size_t max_;
    const std::size_t idleMs_;
    const std::size_t maxQueue_;
    const std::size_t enqueueWaitMs_;

    std::atomic<std::size_t> activeWorkers_;
    std::atomic<bool>        stopped_;

    mutable std::mutex              mtx_;
    std::queue<std::function<void()>> tasks_;
    std::condition_variable          cvWorkers_;
    std::condition_variable          cvQueue_;

    mutable std::mutex       workersMtx_;
    std::vector<std::thread> workers_;

    std::mutex                       deadThreadsMtx_;
    std::unordered_set<std::thread::id> deadThreads_;

    std::thread managerThread_;
};

#endif // DYNAMICTHREADPOOL_H
