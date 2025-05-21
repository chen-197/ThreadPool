#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>
#include <memory>
#include <tuple>

/**
 * @class ThreadPool
 * @brief 基于C++17的高性能线程池实现，支持异常传播和完美参数转发
 * 
 * 特性：
 * - 自动根据硬件并发数初始化线程数量
 * - 支持任意返回类型的任务提交
 * - 异常可通过future传播给调用方
 * - 完美转发函数参数，保持值类别
 * - 线程安全的停止机制
 */
class ThreadPool {
public:
    /**
     * @brief 构造函数，自动启动工作线程
     * @param threads 指定线程数量，默认为硬件支持的并发线程数
     * 
     * @note 当参数为0时自动设置为1个线程，防止空线程池
     */
    explicit ThreadPool(size_t threads = std::thread::hardware_concurrency())
        : stop(false) // 初始化原子停止标志
    {
        // 确保至少一个工作线程
        if (threads == 0) threads = 1;
        workers.reserve(threads);

        // 创建工作线程
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                // 线程主循环
                while (true) {
                    std::function<void()> task;

                    // 临界区：访问任务队列
                    {
                        std::unique_lock<std::mutex> lock(queue_mutex);
                        
                        // 等待条件：停止标志或非空队列（避免虚假唤醒）
                        condition.wait(lock, [this] { 
                            return stop || !tasks.empty(); 
                        });

                        // 终止条件：停止标志且队列为空
                        if (stop && tasks.empty()) return;

                        // 提取队列首部任务
                        task = std::move(tasks.front());
                        tasks.pop();
                    }

                    // 执行任务（异常将传播到future）
                    task();
                }
            });
        }
    }

    /**
     * @brief 析构函数，安全停止所有线程
     * 
     * @note 采用RAII方式管理线程资源：
     * 1. 设置停止标志
     * 2. 唤醒所有等待线程
     * 3. 等待所有线程执行完毕
     */
    ~ThreadPool() {
        // 设置停止标志（临界区保护）
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }

        // 唤醒所有等待线程
        condition.notify_all();

        // 等待线程终止
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    /**
     * @brief 提交任务到线程池
     * @tparam F 可调用对象类型
     * @tparam Args 参数类型包
     * @param f 可调用对象
     * @param args 调用参数（支持完美转发）
     * @return std::future<return_type> 用于获取异步结果的future对象
     * 
     * @throws std::runtime_error 当线程池已停止时提交任务
     * 
     */
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result_t<F, Args...>>
    {
        using return_type = typename std::invoke_result_t<F, Args...>;
        
        // 编译期分支处理void返回类型
        if constexpr (std::is_void_v<return_type>) {
            // 创建任务包装器（完美转发参数）
            auto task = std::make_shared<std::packaged_task<return_type()>>(
                [func = std::forward<F>(f),  // 捕获可调用对象
                 args = std::make_tuple(std::forward<Args>(args)...)]() // 捕获参数包
                {
                    std::apply(func, args);  // 完美转发参数调用
                }
            );
            
            std::future<return_type> res = task->get_future();
            
            // 临界区：添加任务到队列
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                if (stop) {
                    throw std::runtime_error("Enqueue on stopped ThreadPool");
                }
                tasks.emplace([task]() { (*task)(); }); // 类型擦除为void()
            }
            
            condition.notify_one();
            return res;

        } else {
            // 非void返回类型处理（同上）
            auto task = std::make_shared<std::packaged_task<return_type()>>(
                [func = std::forward<F>(f), 
                 args = std::make_tuple(std::forward<Args>(args)...)]() 
                { 
                    return std::apply(func, args); 
                }
            );
            
            std::future<return_type> res = task->get_future();
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                if (stop) throw std::runtime_error("Enqueue on stopped ThreadPool");
                tasks.emplace([task]() { (*task)(); });
            }
            condition.notify_one();
            return res;
        }
    }

    /**
     * @brief 获取当前线程池大小
     * @return size_t 工作线程数量
     */
    size_t getPoolSize() const { 
        return workers.size(); 
    }

    // 删除拷贝和移动操作
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

private:
    /// @brief 工作线程容器
    std::vector<std::thread> workers;
    
    /// @brief 任务队列（存储类型擦除的可调用对象）
    std::queue<std::function<void()>> tasks;

    /// @brief 保护任务队列的互斥锁
    std::mutex queue_mutex;
    
    /// @brief 用于线程同步的条件变量
    std::condition_variable condition;
    
    /// @brief 原子停止标志
    bool stop;
};

#endif // THREADPOOL_H