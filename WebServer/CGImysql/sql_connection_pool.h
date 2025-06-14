#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <iostream>
#include <string.h>
#include <error.h>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

//使用局部静态变量懒汉模式创建连接池
class connection_pool{
public:
    MYSQL *GetConnection();              //获取数据库连接
    bool ReleaseConnection(MYSQL *conn); //释放连接
    int GetFreeConn();                   //获取连接
    void DestroyPool();                  //销毁所有连接
    //单例模式
    static connection_pool *GetInstance();

    void init(string url,string User,string PassWord,string DataBaseName,int Port,int MaxConn,int close_log); 
private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn; //最大连接数
    int m_CurConn; //当前已使用连接数
    int m_FreeConn;//空闲连接数
    locker lock;
    list<MYSQL *>connList; //连接池
    sem reserve;
public:
    string m_url;             //主机地址
    string m_Port;          //数据库端口号
    string m_User;              
    string m_PassWord;
    string m_DatabaseName;
    int m_close_log;        // 日志开关
};

class connectionRAII{
public:
    connectionRAII(MYSQL **con,connection_pool *connPool);
    ~connectionRAII();
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif