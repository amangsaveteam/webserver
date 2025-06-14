#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}
void sort_timer_lst::add_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    if(!head)
    {
        head = tail =timer;
        return;
    }
    //如果新的定时器超过时间小于当前头部结点
    //直接将当前定时器结点作为头部结点
    if(timer->expire < head->expire)
    {
        timer->next  = head;
        head->prev = timer;
        head = timer;
        return;
    }
    //否则调用私有成员，调整内部结点
    add_timer(timer,head);
}
//调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer* timer)
{
    if(!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    //被调整的定时器在链表尾部
    //定时器超时值仍然小于下一个定时器超时值
    if(!tmp || (timer->expire < tmp ->expire))
    {
        return;
    }
    //被调整定时器是链表头结点，将定时器取出，重新插入
    if(timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer,head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer,timer->next);
    }
}
//删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if(!timer)
    {
        return;
    }
    //链表中只有一个定时器，删除该定时器
    if((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail == NULL;
        return;
    }
    //如果被删除结点为头结点
    if(timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //被删除的定时器为尾结点
    if(timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return; 
    }
    //被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
//检查是否有超时结点
void sort_timer_lst::tick()
{
    if(!head)
    {
        return;
    }
    time_t cur = time(NULL);
    util_timer *tmp = head;
    while(tmp)
    {
        if(cur < tmp->expire)
        {
            break;
        }
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if(head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
//调整链表内部结构
void sort_timer_lst::add_timer(util_timer *timer,util_timer* lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp  = prev->next;
    //遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    while(tmp)
    {
        if(timer->expire <tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev =tmp;
        tmp = tmp->next;
    }
    //遍历完，发现定时器需要放到尾结点处
    if(!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
} 
//文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}
//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else    
        event.events = EPOLLIN | EPOLLRDHUP;
    if(one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}
//信号处理函数
void Utils::sig_handler(int sig)
{
    //为保证函数可重入性,保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1],(char *)&msg,1,0);
    errno = save_errno;
}
//设置信号函数
void Utils::addsig(int sig,void(handler)(int),bool restart) 
{
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    if(restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig,&sa,NULL) != -1);
}
//定时处理任务，重新定时以不断触发SIGALARM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}
void Utils::show_error(int connfd,const char *info)
{
    send(connfd,info,strlen(info),0);
    close(connfd);
}
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}