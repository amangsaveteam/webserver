#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

//信号量
class sem{
    public:
    sem()
    {
        if(sem_init(&m_sem,0,0) != 0)
        {
            throw std::exception();
        }
        
    }
    sem(int num)
    {
        if(sem_init(&m_sem,0,num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }
    private:
    sem_t m_sem;
};
//互斥锁
class locker
{
    public:
        locker()
        {
            if(pthread_mutex_init(&m_mutex,NULL)!=0)
            {
                throw std::exception();
            }
        }
        ~locker()
        {
             pthread_mutex_destroy(&m_mutex);
        }
        bool lock()
        {
            return pthread_mutex_lock(&m_mutex) == 0;
        }
        bool unlock()
        {
            return pthread_mutex_unlock(&m_mutex) == 0;
        }
        pthread_mutex_t *get()
        {
            return &m_mutex;
        }
    private:
        pthread_mutex_t m_mutex;
};
//条件变量
class cond
{   
    public:
        cond(){
            if(pthread_cond_init(&m_cond,NULL) != 0)
            {
                throw std::exception();
            }
        }
        ~cond()
        {
            pthread_cond_destroy(&m_cond);
        }
        //处于等待，直到其他线程通过signal或者breadcast唤醒
        bool wait(pthread_mutex_t *m_mutex)
        {
            int ret=0;
            ret = pthread_cond_wait(&m_cond,m_mutex);
            return ret == 0;
        }
        //有时间限制的等待
        bool timewait(pthread_mutex_t *m_mutex,struct timespec t)
        {
            int ret=0;
            ret = pthread_cond_timedwait(&m_cond,m_mutex,&t);
            return ret == 0;
        }
        //随即唤醒单个线程
        bool signal()
        {
            return pthread_cond_signal(&m_cond)==0;
        }
        //唤醒所有正在等待中的线程
        bool broadcast()
        {
            return pthread_cond_broadcast(&m_cond)==0;
        }
    private:
        pthread_cond_t m_cond;
};


#endif