#ifndef THREADPOOL_H
#define THREADPOOL_H
#include<vector>
#include<unordered_map>
#include<queue>
#include<memory>
#include<atomic>
#include<mutex>
#include<condition_variable>
#include<functional>
#include<chrono>
#include<thread>
#include<future>
#include<iostream>
const int TASK_MAX_THRESHHOLD = 2;
const int THREAD_MAX_THRESHHOLD = 10;
const int THREAD_MAX_IDLE_TIME = 10;//秒
//线程模式
enum class PoolMode {//枚举类 class 解决枚举类型不同，但枚举项相同发生冲突
    MODE_FIXED,//固定线程数
    MODE_CACHED,//线程数量可动态增长
};

//线程类型
class Thread {
public:
    using ThreadFunc = std::function<void(int)>;

    //构造函数
    Thread(ThreadFunc func)
        :func_(func)
        , threadId_(generateId++)
    {}

    //析构函数
    ~Thread() = default;

    //线程启动函数
    void start() {
        std::thread t(func_, threadId_);//C++11，线程对象t,和线程函数func_
        t.detach();//线程分离 phtread_detach
    }

    //获取线程id
    int getId()const {
        return threadId_;
    }
private:
    ThreadFunc func_;//线程执行函数
    static int generateId;
    int threadId_;//线程id
};
int Thread::generateId = 0;

/*
example:
ThreadPool pool;
pool.start(4);
class myTaks: public Task {
    void run() override {
        //do something
    }
};
pool.submitTask(std::make_shared<myTask>());

*/
//线程池类
class ThreadPool {
public:
    ThreadPool() 
        :initThreadSize_(4)
        , curThreadSize_(0)
        , taskSize_(0)
        , taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
        , threadSizeMaxThreshHold_(THREAD_MAX_THRESHHOLD)
        , poolMode_(PoolMode::MODE_FIXED)
        , isPoolRunning_(false)
        , idleThreadSize_(0)
    {}

    ~ThreadPool()
    {
        isPoolRunning_ = false;
        //等待线程池中线程执行完任务，两种状态，阻塞&正在执行任务中
        std::unique_lock<std::mutex> lock(taskQueMutex_);
        notEmpty_.notify_all();
        exitCond_.wait(lock, [&]()->bool {return threads_.size() == 0; });
    }

    //启动线程池
    void start(int size = std::thread::hardware_concurrency()) {
        //设置线程池的启动状态
        isPoolRunning_ = true;
        //记录线程池初始线程数
        initThreadSize_ = size;
        //线程池中线程总数量
        curThreadSize_ = size;
        //创建线程对象,把线程函数绑定给线程对象
        for (int i = 0; i < size; i++) {
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            threads_.emplace(ptr->getId(), std::move(ptr));
        }
        //启动线程对象
        for (int i = 0; i < size; i++) {
            threads_[i]->start();
            idleThreadSize_++;//记录线程池中空闲线程数
        }
    }

    //设置线程模式
    void setMode(PoolMode mode) {
        if (checkPoolState()) {
            return;
        }
        poolMode_ = mode;
    }

    //设置初始线程数
    //void setInitThreadSize(int size);

    //设置任务队列上限阈值
    void setTaskQueMaxThreshHold(int threshHold) {
        taskQueMaxThreshHold_ = threshHold;
    }

    //提交任务
    //使用可变参模板编程，让submitTask支持任意任务函数和任意数量的参数
    //Result submitTask(std::shared_ptr<Task> task);
    //返回值future<?>
    template<typename Func, typename... Args>
    auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))> {
        //打包任务，放入任务队列
        using RType = decltype(func(args...));
        auto task = std::make_shared<std::packaged_task<RType()>>(
            std::bind(std::forward<Func>(func), std::forward<Args>(args)...)
        );
        std::future<RType> result = task->get_future();
        //获取锁
        std::unique_lock<std::mutex> lock(taskQueMutex_);
        //用户提交任务，最长不能阻塞超过1s，否则提交失败，返回
        if (!notFull_.wait_for(lock, std::chrono::seconds(1),
            [&]()->bool {return taskQue_.size() < taskQueMaxThreshHold_; }))
        {
            //表示notFull_等待1s，条件依然没被满足
            std::cerr << "task queue is full, submit task fail" << std::endl;
            task = std::make_shared<std::packaged_task<RType()>>([]()->RType {return RType(); });
            //运行任务,为何不用task->get_future()
            auto future = task->get_future();
            (*task)();
            return future;
        }
        //如果有余，把任务放进任务队列中
        //using task = std::function<void()>;
        //增加一个中间层，将实际任务包装成无返回值无参数的lambda函数对象，执行匿名对象时真正执行task
        //不能使用引用捕获，因为函数结束时task就销毁了，task引用指向被释放的内存。
        // 值捕获相当于将task对象拷贝一份，函数结束时拷贝的task对象还在
        taskQue_.emplace([task]() {(*task)(); });
        taskSize_++;
        //因为新放了任务，队列不空了，在notEmpty上进行通知,赶快分配线程执行任务
        notEmpty_.notify_all();
        //return task->getTask();//任务执行结束，task就销毁了，所以这种不可取

        //cached模式：任务处理比较紧急，场景：小而快的任务。需要根据任务数量和空闲线程数量，判断是否需要创建新线程
        if (poolMode_ == PoolMode::MODE_CACHED && taskSize_ > idleThreadSize_ && curThreadSize_ < threadSizeMaxThreshHold_) {
            //创建新线程
            auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
            int ptrId = ptr->getId();
            threads_.emplace(ptrId, std::move(ptr));
            std::cout << "create new thread..." << std::endl;
            //启动新线程
            threads_[ptrId]->start();
            //修改相关线程数量
            curThreadSize_++;
            idleThreadSize_++;
        }
        return result;
    }
    //设置cached模式下，线程池线程数量的上限
    void setThreadSizeMaxThreshHold(int threshHold) {
        if (checkPoolState()) {
            return;
        }
        if (poolMode_ == PoolMode::MODE_CACHED) {
            threadSizeMaxThreshHold_ = threshHold;
        }
    }

    //不需要拷贝构造和赋值运算符
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
private:
    //线程运行函数
    void threadFunc(int threadId) {
        auto lastTime = std::chrono::high_resolution_clock::now();
        for (;;) {
            Task task;
            {
                //获取锁
                std::unique_lock<std::mutex> lock(taskQueMutex_);
                //cached模式下，有可能已经创建了很多线程，但是空闲时间超过60s，应该把多余的线程回收掉
                //锁加双重判断
                while (taskQue_.size() == 0) {
                    if (!isPoolRunning_) {
                        threads_.erase(threadId);
                        std::cout << "tid:" << std::this_thread::get_id() << "thread is recycled..." << std::endl;
                        exitCond_.notify_all();
                        return;
                    }
                    if (poolMode_ == PoolMode::MODE_CACHED) {
                        if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
                            auto now = std::chrono::high_resolution_clock::now();
                            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                            if (duration.count() > THREAD_MAX_IDLE_TIME && curThreadSize_ > initThreadSize_) {
                                //空闲时间超过60s，回收该线程
                                //记录线程数量的变量修改
                                curThreadSize_--;
                                idleThreadSize_--;
                                //把线程对象从线程池中删除,没有办法确定threadFunc对应哪一个thread对象
                                //threadid=》线程对象=》删除
                                threads_.erase(threadId);
                                std::cout << "tid:" << std::this_thread::get_id() << "thread is recycled..." << std::endl;
                                return;//线程结束
                            }
                        }
                    }
                    else {
                        //线程通信,等待notEmpty条件
                        notEmpty_.wait(lock);
                    }
                   
                }
                //线程获取任务，线程池中空闲线程数减1
                idleThreadSize_--;
                std::cout << "tid:" << std::this_thread::get_id() << "获取任务成功..." << std::endl;
                //获取任务从任务队列中
                task = taskQue_.front();
                taskQue_.pop();
                taskSize_--;
                //如果任务队列有任务，则通知其他线程执行任务
                if (taskSize_ > 0) {
                    notEmpty_.notify_all();
                }
                //取出任务，通知可以继续提交任务
                notFull_.notify_all();
            }//释放锁,当前线程拿到任务后，就释放锁，交给其他线程获取任务
            //当前线程负责执行这个任务
            if (task != nullptr) {
                task();
            }
            //线程执行完任务，空闲线程数加1
            idleThreadSize_++;
            //更新线程执行玩的任务时间
            lastTime = std::chrono::high_resolution_clock::now();
        }
        /*threads_.erase(threadId);
        std::cout << "tid:" << std::this_thread::get_id() << "线程被回收..." << std::endl;
        exitCond_.notify_all();*/
    }

    //检查Pool的启动状态
    bool checkPoolState() {
        return isPoolRunning_;
    }
private:
    //std::vector<std::unique_ptr<Thread>> threads_;//线程列表    std::vector<Thread*> threads_; ，修改后避免手动释放内存
    std::unordered_map<int, std::unique_ptr<Thread>> threads_;//为了可以释放空闲的线程，有vector-》unordered_map
    int initThreadSize_;//初始线程数
    int threadSizeMaxThreshHold_;//线程数量的上限阈值
    std::atomic_int curThreadSize_;//当前线程池中线程总数量
    std::atomic_int idleThreadSize_;//表示处于空闲状态的线程数

    //Task任务=》函数对象
    using Task = std::function<void()>;
    std::queue<Task> taskQue_;//任务队列(需要考虑用户提交的任务生命周期)shared_ptr<Task>
    std::atomic_int taskSize_;//任务数量
    int taskQueMaxThreshHold_;//任务队列数量上限阈值

    std::mutex taskQueMutex_;//任务队列锁
    std::condition_variable notFull_;//任务队列非满条件变量
    std::condition_variable notEmpty_;//任务队列非空条件变量
    std::condition_variable exitCond_;//线程池关闭条件变量

    PoolMode poolMode_;//线程池工作模式
    std::atomic_bool isPoolRunning_; //表示当前线程池的启动状态

};

#endif