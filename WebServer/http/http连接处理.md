# http连接处理
## epoll
-- epoll_create函数
```c
#include <sys/epoll.h>
int epoll_create(int size)
```
创建一个指示epoll内核事件表的文件描述符，该描述符将用作其他epoll系统调用的第一个参数，size不起作用。
-- epoll_ctl函数
```c
#include <sys/epoll.h>
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
```
该函数用于操作内核事件表监控的文件描述符上的事件：注册、修改、删除
epfd：为epoll_creat的句柄
op：表示动作，用3个宏来表示：
    EPOLL_CTL_ADD (注册新的fd到epfd)，
    EPOLL_CTL_MOD (修改已经注册的fd的监听事件)，
    EPOLL_CTL_DEL (从epfd删除一个fd)；

-- event：告诉内核需要监听的事件
上述event是epoll_event结构体指针类型，表示内核所监听的事件，具体定义如下：
```c
struct epoll_event {
__uint32_t events; /* Epoll events */
epoll_data_t data; /* User data variable */
};
```
events描述事件类型，其中epoll事件类型有以下几种
    EPOLLIN：表示对应的文件描述符可以读（包括对端SOCKET正常关闭）
    EPOLLOUT：表示对应的文件描述符可以写
    EPOLLPRI：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）
    EPOLLERR：表示对应的文件描述符发生错误
    EPOLLHUP：表示对应的文件描述符被挂断；
    EPOLLET：将EPOLL设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)而言的
    EPOLLONESHOT：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里

-- epoll_wait函数
```c
#include <sys/epoll.h>
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
```
该函数用于等待所监控文件描述符上有事件的产生，返回就绪的文件描述符个数
    events：用来存内核得到事件的集合，
    maxevents：告之内核这个events有多大，这个maxevents的值不能大于创建epoll_create()时的size，
    timeout：是超时时间
        -1：阻塞
        0：立即返回，非阻塞
        >0：指定毫秒
    返回值：成功返回有多少文件描述符就绪，时间到时返回0，出错返回-1

## select/poll/epoll
    -- 调用函数
        select和poll都是一个函数，epoll是一组函数

    -- 文件描述符数量
        select通过线性表描述文件描述符集合，文件描述符有上限，一般是1024，但可以修改源码，重新编译内核，不推荐
        poll是链表描述，突破了文件描述符上限，最大可以打开文件的数目
        epoll通过红黑树描述，最大可以打开文件的数目，可以通过命令ulimit -n number修改，仅对当前终端有效

    -- 将文件描述符从用户传给内核
        select和poll通过将所有文件描述符拷贝到内核态，每次调用都需要拷贝
        epoll通过epoll_create建立一棵红黑树，通过epoll_ctl将要监听的文件描述符注册到红黑树上

    -- 内核判断就绪的文件描述符
        select和poll通过遍历文件描述符集合，判断哪个文件描述符上有事件发生
        epoll_create时，内核除了帮我们在epoll文件系统里建了个红黑树用于存储以后epoll_ctl传来的fd外，还会再建立一个list链表，用于存储准备就绪的事件，当epoll_wait调用时，仅仅观察这个list链表里有没有数据即可。
        epoll是根据每个fd上面的回调函数(中断函数)判断，只有发生了事件的socket才会主动的去调用 callback函数，其他空闲状态socket则不会，若是就绪事件，插入list

    -- 应用程序索引就绪文件描述符
        select/poll只返回发生了事件的文件描述符的个数，若知道是哪个发生了事件，同样需要遍历
        epoll返回的发生了事件的个数和结构体数组，结构体包含socket的信息，因此直接处理返回的数组即可

    -- 工作模式
        select和poll都只能工作在相对低效的LT模式下
        epoll则可以工作在ET高效模式，并且epoll还支持EPOLLONESHOT事件，该事件能进一步减少可读、可写和异常事件被触发的次数。 

    -- 应用场景
        当所有的fd都是活跃连接，使用epoll，需要建立文件系统，红黑书和链表对于此来说，效率反而不高，不如selece和poll
        当监测的fd数目较小，且各个fd都比较活跃，建议使用select或者poll
        当监测的fd数目非常大，成千上万，且单位时间只有其中的一部分fd处于就绪状态，这个时候使用epoll能够明显提升性能
## ET、LT、EPOLLONESHOT
    -- LT水平触发模式
        epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序可以不立即处理该事件。
        当下一次调用epoll_wait时，epoll_wait还会再次向应用程序报告此事件，直至被处理

    -- ET边缘触发模式
        epoll_wait检测到文件描述符有事件发生，则将其通知给应用程序，应用程序必须立即处理该事件
        必须要一次性将数据读取完，使用非阻塞I/O，读取到出现eagain

    -- EPOLLONESHOT
        一个线程读取某个socket上的数据后开始处理数据，在处理过程中该socket上又有新数据可读，此时另一个线程被唤醒读取，此时出现两个线程处理同一个socket
        我们期望的是一个socket连接在任一时刻都只被一个线程处理，通过epoll_ctl对该文件描述符注册epolloneshot事件，一个线程处理socket时，其他线程将无法处理，当该线程处理完后，需要通过epoll_ctl重置epolloneshot事件
## http报文格式
HTTP报文分为请求报文和响应报文两种，每种报文必须按照特有格式生成，才能被浏览器端识别。
其中，浏览器端向服务器发送的为请求报文，服务器处理后返回给浏览器端的为响应报文。
### 请求报文
HTTP请求报文由请求行（request line）、请求头部（header）、空行和请求数据四个部分组成。
其中，请求分为两种，GET和POST，具体的：
GET
```
    GET /562f25980001b1b106000338.jpg HTTP/1.1
    Host:img.mukewang.com
    User-Agent:Mozilla/5.0 (Windows NT 10.0; WOW64)
    AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.106 Safari/537.36
    Accept:image/webp,image/*,*/*;q=0.8
    Referer:http://www.imooc.com/
    Accept-Encoding:gzip, deflate, sdch
    Accept-Language:zh-CN,zh;q=0.8
    空行
    请求数据为空
```
POST
```
    POST / HTTP1.1
    Host:www.wrox.com
    User-Agent:Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1; SV1; .NET CLR 2.0.50727; .NET CLR 3.0.04506.648; .NET CLR 3.5.21022)
    Content-Type:application/x-www-form-urlencoded
    Content-Length:40
    Connection: Keep-Alive
    空行
    name=Professional%20Ajax&publisher=Wiley
```
请求行，用来说明请求类型,要访问的资源以及所使用的HTTP版本。
GET说明请求类型为GET，/562f25980001b1b106000338.jpg(URL)为要访问的资源，该行的最后一部分说明使用的是HTTP1.1版本。
请求头部，紧接着请求行（即第一行）之后的部分，用来说明服务器要使用的附加信息。
    HOST，给出请求资源所在服务器的域名。
    User-Agent，HTTP客户端程序的信息，该信息由你发出请求使用的浏览器来定义,并且在每个请求中自动发送等。
    Accept，说明用户代理可处理的媒体类型。
    Accept-Encoding，说明用户代理支持的内容编码。
    Accept-Language，说明用户代理能够处理的自然语言集。
    Content-Type，说明实现主体的媒体类型
    Content-Length，说明实现主体的大小。
    Connection，连接管理，可以是Keep-Alive或close
空行，请求头部后面的空行是必须的即使第四部分的请求数据为空，也必须有空行。
请求数据也叫主体，可以添加任意的其他数据。
### 响应报文
HTTP响应也由四个部分组成，分别是：状态行、消息报头、空行和响应正文
```
HTTP/1.1 200 OK
Date: Fri, 22 May 2009 06:07:21 GMT
Content-Type: text/html; charset=UTF-8
空行
<html>
    <head></head>
    <body>
        <!--body goes here-->
    </body>
</html>
```
状态行，由HTTP协议版本号， 状态码， 状态消息 三部分组成。
    第一行为状态行，（HTTP/1.1）表明HTTP版本为1.1版本，状态码为200，状态消息为OK。
消息报头，用来说明客户端要使用的一些附加信息。
    第二行和第三行为消息报头，Date:生成响应的日期和时间；Content-Type:指定了MIME类型的HTML(text/html),编码类型是UTF-8。
空行，消息报头后面的空行是必须的。
响应正文，服务器返回给客户端的文本信息。空行后面的html部分为响应正文。
### http状态码
HTTP有5种类型的状态码，具体的：
    1xx：指示信息--表示请求已接收，继续处理。

    2xx：成功--表示请求正常处理完毕。
        200 OK：客户端请求被正常处理。
        206 Partial content：客户端进行了范围请求。

    3xx：重定向--要完成请求必须进行更进一步的操作。
        301 Moved Permanently：永久重定向，该资源已被永久移动到新位置，将来任何对该资源的访问都要使用本响应返回的若干个URI之一。
        302 Found：临时重定向，请求的资源现在临时从不同的URI中获得。

    4xx：客户端错误--请求有语法错误，服务器无法处理请求。
        400 Bad Request：请求报文存在语法错误。
        403 Forbidden：请求被服务器拒绝。
        404 Not Found：请求不存在，服务器上找不到请求的资源。

    5xx：服务器端错误--服务器处理请求出错。
        500 Internal Server Error：服务器在执行请求时出现错误。
### 有限状态机
有限状态机，是一种抽象的理论模型，它能够把有限个变量描述的状态变化过程，以可构造可验证的方式呈现出来。比如，封闭的有向图。
有限状态机可以通过if-else,switch-case和函数指针来实现，从软件工程的角度看，主要是为了封装逻辑。
```cpp
STATE_MACHINE(){
   State cur_State = type_A;
   while(cur_State != type_C){
        Package _pack = getNewPackage();
        switch(){
            case type_A:
                process_pkg_state_A(_pack);
                cur_State = type_B;
                break;
            case type_B:
                process_pkg_state_B(_pack);
                cur_State = type_C;
                break;
        }
    }
}
```
该状态机包含三种状态：type_A，type_B和type_C。其中，type_A是初始状态，type_C是结束状态。
状态机的当前状态记录在cur_State变量中，逻辑处理时，状态机先通过getNewPackage获取数据包，然后根据当前状态对数据进行处理，处理完后，状态机通过改变cur_State完成状态转移。
有限状态机一种逻辑单元内部的一种高效编程方法，在服务器编程中，服务器可以根据不同状态或者消息类型进行相应的处理逻辑，使得程序逻辑清晰易懂。
## http处理流程
### http报文处理流程
浏览器端发出http连接请求，主线程创建http对象接收请求并将所有数据读入对应buffer，将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
工作线程取出任务后，调用process_read函数，通过主、从状态机对请求报文进行解析。
解析完之后，跳转do_request函数生成响应报文，通过process_write写入buffer，返回给浏览器端。
### http类
在http请求接收部分，会涉及到init和read_once函数，但init仅仅是对私有成员变量进行初始化，不用过多讲解。
这里，对read_once进行介绍。read_once读取浏览器端发送来的请求报文，直到无数据可读或对方关闭连接，读取到m_read_buffer中，并更新m_read_idx。
# 流程图与状态机
从状态机负责读取报文的一行，主状态机负责对该行数据进行解析，主状态机内部调用从状态机，从状态机驱动主状态机。
![alt text](c4440f5b-33c7-4696-bd37-8f0bf5a3cd6f.png)
## 主状态机
三种状态，标识解析位置。
    CHECK_STATE_REQUESTLINE，解析请求行
    CHECK_STATE_HEADER，解析请求头
    CHECK_STATE_CONTENT，解析消息体，仅用于解析POST请求
## 从状态机
三种状态，标识解析一行的读取状态。
    LINE_OK，完整读取一行
    LINE_BAD，报文语法有误
    LINE_OPEN，读取的行不完整
## HTTP_CODE含义
表示HTTP请求的处理结果，在头文件中初始化了八种情形，在报文解析时只涉及到四种。
    NO_REQUEST
        请求不完整，需要继续读取请求报文数据
    GET_REQUEST
        获得了完整的HTTP请求
    BAD_REQUEST
        HTTP请求报文有语法错误
    INTERNAL_ERROR
        服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发

## 解析报文整体流程
process_read通过while循环，将主从状态机进行封装，对报文的每一行进行循环处理。
    判断条件
        主状态机转移到CHECK_STATE_CONTENT，该条件涉及解析消息体
        从状态机转移到LINE_OK，该条件涉及解析请求行和请求头部
        两者为或关系，当条件为真则继续循环，否则退出
    循环体
        从状态机读取数据
        调用get_line函数，通过m_start_line将从状态机读取数据间接赋给text
        主状态机解析text
## 从状态机逻辑
在HTTP报文中，每一行的数据由\r\n作为结束字符，空行则是仅仅是字符\r\n。因此，可以通过查找\r\n将报文拆解成单独的行进行解析，项目中便是利用了这一点。

从状态机负责读取buffer中的数据，将每行数据末尾的\r\n置为\0\0，并更新从状态机在buffer中读取的位置m_checked_idx，以此来驱动主状态机解析。
    从状态机从m_read_buf中逐字节读取，判断当前字节是否为\r
        接下来的字符是\n，将\r\n修改成\0\0，将m_checked_idx指向下一行的开头，则返回LINE_OK
        接下来达到了buffer末尾，表示buffer还需要继续接收，返回LINE_OPEN
        否则，表示语法错误，返回LINE_BAD
    当前字节不是\r，判断是否是\n（一般是上次读取到\r就到了buffer末尾，没有接收完整，再次接收时会出现这种情况）
        如果前一个字符是\r，则将\r\n修改成\0\0，将m_checked_idx指向下一行的开头，则返回LINE_OK
    当前字节既不是\r，也不是\n
        表示接收不完整，需要继续接收，返回LINE_OPEN
## 主状态机逻辑
主状态机初始状态是CHECK_STATE_REQUESTLINE，通过调用从状态机来驱动主状态机，在主状态机进行解析前，从状态机已经将每一行的末尾\r\n符号改为\0\0，以便于主状态机直接取出对应字符串进行处理。
    CHECK_STATE_REQUESTLINE
        主状态机的初始状态，调用parse_request_line函数解析请求行
        解析函数从m_read_buf中解析HTTP请求行，获得请求方法、目标URL及HTTP版本号
        解析完成后主状态机的状态变为CHECK_STATE_HEADER
解析完请求行后，主状态机继续分析请求头。在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
    CHECK_STATE_HEADER
        调用parse_headers函数解析请求头部信息
        判断是空行还是请求头，若是空行，进而判断content-length是否为0，如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，否则说明是GET请求，则报文解析结束。
        若解析的是请求头部字段，则主要分析connection字段，content-length字段，其他字段可以直接跳过，各位也可以根据需求继续分析。
        connection字段判断是keep-alive还是close，决定是长连接还是短连接
        content-length字段，这里用于读取post请求的消息体长度
如果仅仅是GET请求，如项目中的欢迎界面，那么主状态机只设置之前的两个状态足矣。
GET和POST请求报文的区别之一是有无消息体部分，GET请求没有消息体，当解析完空行之后，便完成了报文的解析。
但后续的登录和注册功能，为了避免将用户名和密码直接暴露在URL中，我们在项目中改用了POST请求，将用户名和密码添加在报文中作为消息体进行了封装。
为此，我们需要在解析报文的部分添加解析消息体的模块。
```cpp
while((m_check_state==CHECK_STATE_CONTENT && line_status==LINE_OK)||((line_status=parse_line())==LINE_OK))
```
### 判断条件为什么要写成这样呢？
在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。
但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，这里转而使用主状态机的状态作为循环入口条件。

### 后面的&& line_status==LINE_OK又是为什么？
解析完消息体后，报文的完整解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，也就是说，符合循环入口条件，还会再次进入循环，这并不是我们所希望的。
为此，增加了该语句，并在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务。
    CHECK_STATE_CONTENT
        仅用于解析POST请求，调用parse_content函数解析消息体
        用于保存post请求消息体，为后面的登录和注册做准备
# 响应请求报文，并将该报文发送给浏览器端
## 基础API
### stat
stat函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里
```c
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

//获取文件属性，存储在statbuf中
int stat(const char *pathname, struct stat *statbuf);

struct stat 
{
   mode_t    st_mode;        /* 文件类型和权限 */
   off_t     st_size;        /* 文件大小，字节数*/
};
```
### mmap
用于将一个文件或其他对象映射到内存，提高文件的访问速度
```c
void* mmap(void* start,size_t length,int prot,int flags,int fd,off_t offset);
int munmap(void* start,size_t length);
```
start：映射区的开始地址，设置为0时表示由系统决定映射区的起始地址
length：映射区的长度
prot：期望的内存保护标志，不能与文件的打开模式冲突
    PROT_READ 表示页内容可以被读取
flags：指定映射对象的类型，映射选项和映射页是否可以共享
    MAP_PRIVATE 建立一个写入时拷贝的私有映射，内存区域的写入不会影响到原文件
fd：有效的文件描述符，一般是由open()函数返回
offset：被映射对象内容的起点
### iovec
定义了一个向量元素，通常，这个结构用作一个多元素的数组
```c
struct iovec {
    void      *iov_base;      /* starting address of buffer */
    size_t    iov_len;        /* size of buffer */
};
```
iov_base指向数据的地址
iov_len表示数据的长度
### writev
writev函数用于在一次函数调用中写多个非连续缓冲区，有时也将这该函数称为聚集写。
```c
#include <sys/uio.h>
ssize_t writev(int filedes, const struct iovec *iov, int iovcnt);
```
filedes表示文件描述符
iov为前述io向量机制结构体iovec
iovcnt为结构体的个数

若成功则返回已写的字节数，若出错则返回-1。writev以顺序iov[0]，iov[1]至iov[iovcnt-1]从缓冲区中聚集输出数据。writev返回输出的字节总数，通常，它应等于所有缓冲区长度之和。
## 流程图
![alt text](9a13a657-f795-457b-b805-80aee99633ed.png)
浏览器端发出HTTP请求报文，服务器端接收该报文并调用process_read对其进行解析，根据解析结果HTTP_CODE，进入相应的逻辑和模块。
其中，服务器子线程完成报文的解析与响应；主线程监测读写事件，调用read_once和http_conn::write完成数据的读取与发送。
### HTTP_CODE含义
-- NO_REQUEST
    请求不完整，需要继续读取请求报文数据
    跳转主线程继续监测读事件
-- GET_REQUEST
    获得了完整的HTTP请求
    调用do_request完成请求资源映射
-- NO_RESOURCE
    请求资源不存在
    跳转process_write完成响应报文
-- BAD_REQUEST
    HTTP请求报文有语法错误或请求资源为目录
    跳转process_write完成响应报文
-- FORBIDDEN_REQUEST
    请求资源禁止访问，没有读取权限
    跳转process_write完成响应报文
-- FILE_REQUEST
    请求资源可以正常访问
    跳转process_write完成响应报文
-- INTERNAL_ERROR
    服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
## 代码分析
### do_request
process_read函数的返回值是对请求的文件分析后的结果，一部分是语法错误导致的BAD_REQUEST，一部分是do_request的返回结果.该函数将网站根目录和url文件拼接，然后通过stat判断该文件属性。另外，为了提高访问速度，通过mmap进行映射，将普通文件映射到内存逻辑地址。
为了更好的理解请求资源的访问流程，这里对各种各页面跳转机制进行简要介绍。其中，浏览器网址栏中的字符，即url，可以将其抽象成ip:port/xxx，xxx通过html文件的action属性进行设置。
m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后的m_url有8种情况。
    /
        GET请求，跳转到judge.html，即欢迎访问页面
    /0
        POST请求，跳转到register.html，即注册页面
    /1
        POST请求，跳转到log.html，即登录页面
    /2CGISQL.cgi
        POST请求，进行登录校验
        验证成功跳转到welcome.html，即资源请求成功页面
        验证失败跳转到logError.html，即登录失败页面
    /3CGISQL.cgi
        POST请求，进行注册校验
        注册成功跳转到log.html，即登录页面
        注册失败跳转到registerError.html，即注册失败页面
    /5
        POST请求，跳转到picture.html，即图片请求页面
    /6
        POST请求，跳转到video.html，即视频请求页面
    /7
        POST请求，跳转到fans.html，即关注页面
### process_write
根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文。
    add_status_line函数，添加状态行：http/1.1 状态码 状态消息
    add_headers函数添加消息报头，内部调用add_content_length和add_linger函数
        content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
        connection记录连接状态，用于告诉浏览器端保持长连接
    add_blank_line添加空行
上述涉及的5个函数，均是内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容。
响应报文分为两种，一种是请求文件的存在，通过io向量机制iovec，声明两个iovec，第一个指向m_write_buf，第二个指向mmap的地址m_file_address；一种是请求出错，这时候只申请一个iovec，指向m_write_buf。
    iovec是一个结构体，里面有两个元素，指针成员iov_base指向一个缓冲区，这个缓冲区是存放的是writev将要发送的数据。
    成员iov_len表示实际写入的长度
### http_conn::write
服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
该函数具体逻辑如下：
在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功。
    若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.
        长连接重置http类实例，注册读事件，不关闭连接，
        短连接直接关闭连接
    若writev单次发送不成功，判断是否是写缓冲区满了。
        若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
        若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
