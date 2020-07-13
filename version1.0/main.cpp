#include "requestData.h"
#include "epoll.h"
#include "threadpool.h"
#include "util.h"

#include <sys/epoll.h>
#include <queue>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <unistd.h>

using namespace std;

const int THREADPOOL_THREAD_NUM = 4;
const int QUEUE_SIZE = 65535;

const int PORT = 8888;
const int ASK_STATIC_FILE = 1;
const int ASK_IMAGE_STITCH = 2;

const string PATH = "/";

const int TIMER_TIME_OUT = 500;


extern pthread_mutex_t qlock;
extern struct epoll_event* events;
void acceptConnection(int listen_fd, int epoll_fd, const string& path);

extern priority_queue<mytimer*, deque<mytimer*>, timerCmp> myTimerQueue;

int socket_bind_listen(int port)
{
    // 检查port值，取正确区间范围
    if (port < 1024 || port > 65535)
        return -1;

    // 创建socket(IPv4 + TCP)，返回监听描述符
    int listen_fd = 0;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // 消除bind时"Address already in use"错误
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // 设置服务器IP和Port，和监听描述副绑定
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr*) & server_addr, sizeof(server_addr)) == -1)
        return -1;

    // 开始监听，最大等待队列长为LISTENQ
    if (listen(listen_fd, LISTENQ) == -1)
        return -1;

    // 无效监听描述符
    if (listen_fd == -1)
    {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

void myHandler(void* args)
{
    requestData* req_data = (requestData*)args;
    req_data->handleRequest();
}

void acceptConnection(int listen_fd, int epoll_fd, const string& path)
{
    /*
    struct sockaddr_in{
    short int    sin_family;    //地址族(Address Family)
    unsigned short int    sin_port;    //16位 TCP/UDP端口号
    struct in_addr    sin_addr;    //32位 IP地址
    char    sin_zero[8];    //不使用,对齐作用

    };
    */
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    //accept接收成功之后会返回一个新的socket连接
    while ((accept_fd = accept(listen_fd, (struct sockaddr*) & client_addr, &client_addr_len)) > 0)
    {
        /*
        // TCP的保活机制默认是关闭的
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */

        // 设为非阻塞模式
        int ret = setSocketNonBlocking(accept_fd);
        if (ret < 0)
        {
            perror("Set non block failed!");
            return;
        }
        //为即将开始的http协议解析创建数据结构
        requestData* req_info = new requestData(epoll_fd, accept_fd, path);

        // 文件描述符可以读，边缘触发(Edge Triggered)模式，保证一个socket连接在任一时刻只被一个线程处理
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        //add函数的几个参数分别是，epoll的代号，accept的代号，分配的数据编号，设置好的文件描述符参数
        epoll_add(epoll_fd, accept_fd, static_cast<void*>(req_info), _epo_event);
        // 新增时间信息
        mytimer* mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info->addTimer(mtimer);
        pthread_mutex_lock(&qlock);
        //将其加入优先队列，优先队列有一个特点，每次从尾部加入元素，但会将其中最大/最小值置于队首，其原理是堆排序。此处是小根堆的形式，原因是需要最先处理掉过期及快过期的
        myTimerQueue.push(mtimer);
        pthread_mutex_unlock(&qlock);
    }
    //if(accept_fd == -1)
     //   perror("accept");
}
// 分发处理函数
void handle_events(int epoll_fd, int listen_fd, struct epoll_event* events, int events_num, const string& path, threadpool_t* tp)
{
    for (int i = 0; i < events_num; i++)
    {
        // 获取有事件产生的描述符
        requestData* request = (requestData*)(events[i].data.ptr);
        int fd = request->getFd();

        // 有事件发生的描述符为监听描述符
        if (fd == listen_fd)
        {
            //cout << "This is listen_fd" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        }
        else
        {
            // 排除错误事件
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                || (!(events[i].events & EPOLLIN)))
            {
                printf("error event\n");
                delete request;
                continue;
            }

            // 将请求任务加入到线程池中
            // 加入线程池之前将Timer和request分离
            request->seperateTimer();
            int rc = threadpool_add(tp, myHandler, events[i].data.ptr, 0);
        }
    }
}



void handle_expired_event()
{
    //只检查最顶上的，如果最顶上的过期了就弹出，如果没过期就退出循环
    pthread_mutex_lock(&qlock);
    while (!myTimerQueue.empty())
    {
        mytimer* ptimer_now = myTimerQueue.top();
        //检测是否已经被置为过期
        if (ptimer_now->isDeleted())
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        //如果之前没过期，检查是否过期
        else if (ptimer_now->isvalid() == false)
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        else
        {
            break;
        }
    }
    pthread_mutex_unlock(&qlock);
}

int main()
{
    //将SIGPIPE这个信号处理掉，因为往一个读段关闭的管道或socket连接中写数据会将其引发，当程序接收到
    //此信号时会结束进程
    handle_for_sigpipe();      
    //创建一个文件描述符
    int epoll_fd = epoll_init();
   //如果创建失败返回
    if (epoll_fd < 0)
    {
        perror("epoll init failed");
        return 1;
    }

    threadpool_t* threadpool = threadpool_create(THREADPOOL_THREAD_NUM, QUEUE_SIZE, 0);
    int listen_fd = socket_bind_listen(PORT);
    if (listen_fd < 0)
    {
        perror("socket bind failed");
        return 1;
    }
    //将listen_fd设置为非阻塞，并检查是否成功
    if (setSocketNonBlocking(listen_fd) < 0)
    {
        perror("set socket non block failed");
        return 1;
    }
    //将epoll事件设置如下，epollin（读事件），EPOLLET（边缘触发）
    __uint32_t event = EPOLLIN | EPOLLET;
    requestData* req = new requestData();
    req->setFd(listen_fd);
    epoll_add(epoll_fd, listen_fd, static_cast<void*>(req), event);
    while (true)
    {
        //等待事件的产生
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
        //printf("%zu\n", myTimerQueue.size());        
        if (events_num == 0)
            continue;
        printf("%d\n", events_num);
        //printf("%zu\n", myTimerQueue.size());    
        // else
        //     cout << "one connection has come!" << endl;
        // 遍历events数组，根据监听种类及描述符类型分发操作
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);

        handle_expired_event();
    }
    return 0;
}
