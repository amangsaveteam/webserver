#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string,string> users;
//载入数据库，将数据库中的用户名和密码载入到服务器的map中来，map中的key为用户名，value为密码
void http_conn::initmysql_result(connection_pool *connPool)
{
    //在连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql,connPool);
    //在user表中检索username,passwd数据，浏览端输入
    if(mysql_query(mysql,"SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error: %s\n",mysql_error(mysql));
    }
    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);
   
    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);
    //返回字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);
    //从结果集中获取下一行，将对应的用户名和密码存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
//就绪链表的节点通常是指向红黑树中对应 fd 节点的指针或结构体成员，避免数据冗余
void addfd(int epollfd,int fd,bool one_shot,int TRIGMode)
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
    //非阻塞是 EPOLLET（边缘触发）模式的必要条件，否则可能丢事件或死循环。
    setnonblocking(fd);
} 

//从内核时间表删除描述符
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd,int fd,int ev,int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if(1 == TRIGMode)
        event.events = ev |EPOLLONESHOT | EPOLLET | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
} 

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户数量减一
void http_conn::close_conn(bool real_close)
{
    if(real_close &&(m_sockfd != -1))
    {
        printf("close%d\n",m_sockfd);
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr,char *root,int TRIGMode,
                     int close_log,string user,string passwd,string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd,sockfd,true,m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或者http响应格式出错或访问的文件内容为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user,user.c_str());
    strcpy(sql_passwd,passwd.c_str());
    strcpy(sql_name,sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_have_send = 0;
    bytes_to_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger =false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host =0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag =0;
    improv = 0;

    memset(m_read_buf,'\0',READ_BUFFER_SIZE);
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);
    memset(m_real_file,'\0',FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx < m_read_idx;++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if(temp == '\r')
        {
            if((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if(m_read_buf[m_checked_idx +  1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        if(temp == '\n')
        {
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] =='\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读数据
    if(0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        m_read_idx += bytes_read;
        if(bytes_read <= 0)
        {
            return false;
        }
        return true;
    }

    //ET读数据
    else{
        while(true)
        {
            bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
            //读取失败
            if(bytes_read == -1)
            {
                //无数据可读
                if(errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            //TCP连接关闭
            else if(bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获取请求方法，目标url,及http版本号
//主状态机逻辑
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text," \t");

    //如果没有空格或\t,则报文格式有误
    if(!m_url)
    {
        return BAD_REQUEST;
    }

    //将该位置改为\0,用于将前面数据取出
    *m_url++ = '\0';

    //取出数据，通过GET与POST比较，以确定请求方式
    char *method=text;
    if(strcasecmp(method,"GET")==0)
        m_method = GET;
    else if(strcasecmp(method,"POST")==0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url," \t");

    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url," \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version," \t");

    //仅支持HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1") !=0)
        return BAD_REQUEST;
    
    //对请求资源前7个字符进行判断
    //报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url += 7;
        m_url = strchr(m_url,'/');
    }
    //同样增加https情况
    if(strncasecmp(m_url,"https://",8)==0)
    {
        m_url += 8;
        m_url = strchr(m_url,'/');
    }
    //不带上述两种符号，直接是单独的/或/后面带访问资源
    if(!m_url || m_url[0]!='/')
        return BAD_REQUEST;
    //如果url为/时，显示欢迎界面
    if(strlen(m_url) == 1)
        strcat(m_url,"judge.html");

    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空行还是请求头
    if(text[0] == '\0')
    {
        //判断是GET还是POST
        if(m_content_length != 0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析头部连接字段
    else if(strncasecmp(text,"Connection:",11) == 0)
    {
        text +=  11;
        text += strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            //如果是长连接，将m_linger设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if(strncasecmp(text,"Content-Length:",15)==0)
    {
        text += 15;
        text += strspn(text," \t");
        //请求头中 Content-Length 字段的值转换成整型数字并保存。
        m_content_length = atol(text);
    }
    //解析头部HOST字段
    else if(strncasecmp(text,"HOST:",5)==0)
    {
        text += 5;
        text +=strspn(text," \t");
        m_host = text;
    }
    else{
        printf("oop!unknow header: %s\n",text);
    }
    return NO_REQUEST;

}
//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer是否读取了消息体
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length]='\0';
        //POST请求中最后输入为用户名和密码
        m_string = text;

        return GET_REQUEST;
    }
    return NO_REQUEST;
}


http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机状态，HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    //从状态机能够完整读取一行或者调用 parse_line() 得到的新行也是 OK，主状态机解析消息体
    while((m_check_state==CHECK_STATE_CONTENT&&line_status==LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("%s",text);
        //主状态机三种状态转移逻辑
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                //解析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:
            {
                //解析请求头
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                //完整解析GET请求后，跳转到报文响应函数
                else if(ret == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret = parse_content(text);
                //完整解析POST请求后，跳转到报文响应函数
                if(ret == GET_REQUEST)
                    return do_request();
                
                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                line_status = LINE_OK;
                break;
            }
            default:
                return INTERNAL_ERROR;

        }
    }
    return NO_REQUEST;
}
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
//释放映射
void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
    
}
bool http_conn::write()
{
    int temp =0;
    //int newadd = 0;
    if(bytes_to_send ==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        init();
        return true;
    }
    while(1)
    {
        //将响应报文的状态行，消息头，空行，响应正文发送给浏览器端
        temp = writev(m_sockfd,m_iv,m_iv_count);

        //正常发送，temp为发送字节数
     if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char* format,...)
{
    //如果写入内容超出m_write_buf大小则报错
    if(m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化为传入参数
    va_start(arg_list,format);

    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if(len >= (WRITE_BUFFER_SIZE -1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //更新m_write_idx位置
    m_write_idx += len;
    //清空可变参列表
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    return true;
}
//添加状态行
bool http_conn::add_status_line(int status,const char* title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}
//添加消息报头，文本长度，连接状态，空行
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
//添加响应报文长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}
//添加文本类型
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n","text/html");
}
//添加连接状态，同志浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}
//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}
//添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s",content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        //内部错误 500
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500,error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        }
        //报文语法错误 404
        case BAD_REQUEST:
        {
            add_status_line(404,error_404_title);
            //消息报头
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }
        //资源没有访问权限 403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!strlen(error_403_form))
                return false;
            break;
        }
        //文件存在 200
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            //如果请求资源存在
            if(m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                //将第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                //第二个指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;

                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求资源大小为0，则返回空白的html文件
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                    return false;
            }
        }
        default:
            return false;
    }
    //除了FILE_REQUEST状态外，其余状态只申请一个iovec,指向报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;    
    m_iv_count = 1;
     bytes_to_send = m_write_idx;
    return true;
}
//通过process函数对process_read,process_write函数分别完成报文解析与报文响应任务
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    //请求不完整，需要继续接收请求数据
    if(read_ret == NO_REQUEST)
    {
        //注册并监听读事件
        modfd(m_epollfd,m_sockfd,EPOLLIN,m_TRIGMode);
        return;
    }
    //完成报文响应
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }
    //注册并监听写事件
    modfd(m_epollfd,m_sockfd,EPOLLOUT,m_TRIGMode);
}