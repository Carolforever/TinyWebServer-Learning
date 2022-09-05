#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state); //request类型为http_conn
    bool append_p(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg); //void类型的指针表示无类型指针，可强制转化为任意类型指针
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //信号量表示是否有任务需要处理
    connection_pool *m_connPool;//数据库
    int m_actor_model;          //模型切换
};
template <typename T>
//构造函数
threadpool<T>::threadpool( int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL),m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number]; //创建线程池
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) //循环创建工作线程，并将工作线程按要求进行运行
        {
            delete[] m_threads;
            throw std::exception();
        }
        //pthread有两种状态joinable状态和unjoinable状态
        //一个线程默认的状态是joinable，如果线程是joinable状态
        //当线程函数自己返回退出时或pthread_exit时都不会释放线程所占用堆栈和线程描述符
        //只有当你调用了pthread_join之后这些资源才会被释放
        //若是unjoinable状态的线程，这些资源在线程函数退出时或pthread_exit时自动会被释放
        //unjoinable属性可以在pthread_create时指定，或在线程创建后在线程中pthread_detach自己, 如：pthread_detach(pthread_self())
        //将线程进行分离后，不用单独对工作线程进行回收
        if (pthread_detach(m_threads[i])) 
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
template <typename T>
threadpool<T>::~threadpool() //析构函数
{
    delete[] m_threads;
}
template <typename T>
//reactor模式下的请求入队
//因为需要线程进行读写操作，所以需要传入工作类型参数
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state; //记录是读还是写类型的任务
    //放入任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //信号量+1提醒有任务要处理
    m_queuestat.post();
    return true;
}
template <typename T>
//proactor模式下的请求入队
//因为读写操作由主线程完成，所以无需传入工作类型参数
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template <typename T>
//线程处理函数
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg; //转化指针类型并指向正在使用的对象，即this指针
    //线程池中每一个线程创建时都会调用run()，睡眠在队列中
    pool->run(); 
    return pool;
}
template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait(); //信号量等待，使得线程进入睡眠状态等待任务出现
        m_queuelocker.lock(); //线程唤醒后先加互斥锁
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        //取出工作队列的首个任务
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request)
            continue;
        if (1 == m_actor_model) //事件处理模式为Reactor
        {
            if (0 == request->m_state) //读任务
            {
                if (request->read_once()) //循环从监听的socket上读取客户数据进入读缓冲区，直到无数据可读或对方关闭连接
                {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool); //RAII模式获取连接
                    request->process(); //处理请求报文并将响应报文存入写缓冲区
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1; 
                }
            }
            else //写任务
            {
                if (request->write()) //将响应报文发送到socket的缓冲区
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else //事件处理模式为Proactor，仅有读事件需要线程参与，写事件由主线程处理
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process(); //线程处理请求报文并将响应报文存入写缓冲区
        }
    }
}
#endif
