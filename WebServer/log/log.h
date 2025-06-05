#ifndef LOG_H
#define LOG_H
#include <iostream>
#include <stdio.h>
#include <pthread.h>
#include <string>
#include <stdarg.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    // C++11 ,懒汉模式不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }
    //可选的参数有日志文件/日志缓冲区大小/最大行数以及最长日志队列
    bool init(const char *file_name,int close_log,int log_buf_size = 8192,int split_lines = 5000000,int max_queue_size = 0);

    //异步写日志公有方法，调用私有的async_write_log
    static void  *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }
    //将输出内容按照标准格式整理
    void write_log(int level,const char *format,...);
    //强制刷新缓冲区
    void flush(void); 
//懒汉模式，通过getinstance获取变量
/* 继承父类析构函数要定义为虚函数，否则释放子类内存时 ，只释放父类
            支持 多态删除。
            避免 资源泄漏。 */
private:
    Log();
    virtual ~Log();

    //异步写日志方法
    void *async_write_log()
    {
        string single_log;
        //从阻塞队列中取一条日志内容写入文件
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(),m_fp);
            m_mutex.unlock();
        }
    }
private:
    char dir_name[128]; //文件名
    char log_name[128]; //log文件名
    int m_split_lines; //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count; //日志行数记录
    int m_today; //按天分文件，记录当前时间是哪一天
    FILE *m_fp;     //打开log文件的指针
    char *m_buf;    //输出内容
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async; //是否同步标志位
    locker m_mutex; //同步类（互斥锁）
    int m_close_log; //关闭日志
};


// 宏定义：用于不同类型日志输出
#define LOG_DEBUG(format,...) if(0 == m_close_log) {Log::get_instance()->write_log(0,format,##__VA_ARGS__);Log::get_instance()->flush();}
#define LOG_INFO(format,...) if(0==m_close_log) {Log::get_instance()->write_log(1,format,##__VA_ARGS__);Log::get_instance()->flush();}
#define LOG_WARN(format,...) if(0==m_close_log) {Log::get_instance()->write_log(2,format,##__VA_VARGS__);Log::get_instance()->flush();}
#define LOG_ERROR(format,...) if(0==m_close_log) {Log::get_instance()->write_log(3,format,##__VA_ARGS__);Log::get_instance()->flush();}

#endif 