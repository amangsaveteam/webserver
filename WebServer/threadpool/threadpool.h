#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template<typename T>
class threadpool{
public:
    //thread_number是线程池中线程的数量
    //max_requests是请求队列中最多允许的/等待处理的请求数量
    //connPool是数据库连接池指针
    threadpool(int actor_model,connection_pool *connPool,int thread_number = 8,int max_requests = 10000);
    ~threadpool();

    //向请求队列中插入任务请求
    bool append(T* request,int state);
    bool append_p(T* request);
private:
    static void *worker(void *arg);

    void run();

private:
    //线程池中的线池数
    int m_thread_number;
    //请求队列中允许的最大请求数
    int m_max_requests;
    //描述线程池的数组，其大小为m_thread_number
    pthread_t *m_threads;
    //请求队列
    std::list<T *>m_workqueue;
    //保护请求队列互斥锁
    locker m_queuelocker;
    //是否有任务需要处理
    sem m_queuestat;
    //模型切换
    int m_actor_model;
    //数据库连接池
    connection_pool *m_connPool;
};
//线程池创建与回收
template<typename T>
threadpool<T>::threadpool(int actor_model,connection_pool *connPool,int thread_number ,int max_requests ):m_actor_model(actor_model),m_thread_number(thread_number),m_max_requests(max_requests),m_threads(NULL),m_connPool(connPool)
{
    if(thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    
    //线程id初始化
    m_threads = new pthread_t[m_thread_number];
    if(!m_threads)
        throw std::exception();
    for(int i=0;i<thread_number;++i)
    {
        //循环创建线程，并将工作线程按要求进行运行
        if(pthread_create(m_threads + i,NULL,worker,this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        //将线程进行分离后，不用单独对工作线程进行回收
        if(pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}  
template<typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
} 

//切换状态
template<typename T>
bool threadpool<T>::append(T *request,int state)
{
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    //添加任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    //信号量提醒有任务要处理
    m_queuestat.post();
    return true;
}
//不切换状态插入
template<typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if(m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
template<typename T>
void *threadpool<T>::worker(void *arg)
{
    //将参数强转为线程池类，调用成员方法
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}
//主要实现，工作线程从请求队列中取出某个任务进行处理，注意线程同步。
template<typename T>
void threadpool<T>::run()
{
    while(true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if(m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if(!request)
            continue;
        //任务处理逻辑,0-->reactor,1-->proactor
        if(0 == m_actor_model)
        {
            //检查任务状态0-->read,else-->write
            if(0 == request->m_state)
            {
                if(request->read_once())
                {
                    //improv 是一个标志位，用于表示任务的处理状态或行为
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql,m_connPool);
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag =1;
                }
            }
            else
            {
                if(request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag =1;
                }
            }
        }
        else
        {
            connectionRAII mysqlcon(&request->mysql,m_connPool);
            request->process();
        }
    }
    
}


#endif