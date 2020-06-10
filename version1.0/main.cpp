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
    // ���portֵ��ȡ��ȷ���䷶Χ
    if (port < 1024 || port > 65535)
        return -1;

    // ����socket(IPv4 + TCP)�����ؼ���������
    int listen_fd = 0;
    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;

    // ����bindʱ"Address already in use"����
    int optval = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1)
        return -1;

    // ���÷�����IP��Port���ͼ�����������
    struct sockaddr_in server_addr;
    bzero((char*)&server_addr, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);
    if (bind(listen_fd, (struct sockaddr*) & server_addr, sizeof(server_addr)) == -1)
        return -1;

    // ��ʼ���������ȴ����г�ΪLISTENQ
    if (listen(listen_fd, LISTENQ) == -1)
        return -1;

    // ��Ч����������
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
    short int    sin_family;    //��ַ��(Address Family)
    unsigned short int    sin_port;    //16λ TCP/UDP�˿ں�
    struct in_addr    sin_addr;    //32λ IP��ַ
    char    sin_zero[8];    //��ʹ��,��������

    };
    */
    struct sockaddr_in client_addr;
    memset(&client_addr, 0, sizeof(struct sockaddr_in));
    socklen_t client_addr_len = 0;
    int accept_fd = 0;
    //accept���ճɹ�֮��᷵��һ���µ�socket����
    while ((accept_fd = accept(listen_fd, (struct sockaddr*) & client_addr, &client_addr_len)) > 0)
    {
        /*
        // TCP�ı������Ĭ���ǹرյ�
        int optval = 0;
        socklen_t len_optval = 4;
        getsockopt(accept_fd, SOL_SOCKET,  SO_KEEPALIVE, &optval, &len_optval);
        cout << "optval ==" << optval << endl;
        */

        // ��Ϊ������ģʽ
        int ret = setSocketNonBlocking(accept_fd);
        if (ret < 0)
        {
            perror("Set non block failed!");
            return;
        }
        //Ϊ������ʼ��httpЭ������������ݽṹ
        requestData* req_info = new requestData(epoll_fd, accept_fd, path);

        // �ļ����������Զ�����Ե����(Edge Triggered)ģʽ����֤һ��socket��������һʱ��ֻ��һ���̴߳���
        __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
        //add�����ļ��������ֱ��ǣ�epoll�Ĵ��ţ�accept�Ĵ��ţ���������ݱ�ţ����úõ��ļ�����������
        epoll_add(epoll_fd, accept_fd, static_cast<void*>(req_info), _epo_event);
        // ����ʱ����Ϣ
        mytimer* mtimer = new mytimer(req_info, TIMER_TIME_OUT);
        req_info->addTimer(mtimer);
        pthread_mutex_lock(&qlock);
        //����������ȶ��У����ȶ�����һ���ص㣬ÿ�δ�β������Ԫ�أ����Ὣ�������/��Сֵ���ڶ��ף���ԭ���Ƕ����򡣴˴���С���ѵ���ʽ��ԭ������Ҫ���ȴ�������ڼ�����ڵ�
        myTimerQueue.push(mtimer);
        pthread_mutex_unlock(&qlock);
    }
    //if(accept_fd == -1)
     //   perror("accept");
}
// �ַ�������
void handle_events(int epoll_fd, int listen_fd, struct epoll_event* events, int events_num, const string& path, threadpool_t* tp)
{
    for (int i = 0; i < events_num; i++)
    {
        // ��ȡ���¼�������������
        requestData* request = (requestData*)(events[i].data.ptr);
        int fd = request->getFd();

        // ���¼�������������Ϊ����������
        if (fd == listen_fd)
        {
            //cout << "This is listen_fd" << endl;
            acceptConnection(listen_fd, epoll_fd, path);
        }
        else
        {
            // �ų������¼�
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)
                || (!(events[i].events & EPOLLIN)))
            {
                printf("error event\n");
                delete request;
                continue;
            }

            // ������������뵽�̳߳���
            // �����̳߳�֮ǰ��Timer��request����
            request->seperateTimer();
            int rc = threadpool_add(tp, myHandler, events[i].data.ptr, 0);
        }
    }
}

/* �����߼���������~
��Ϊ(1) ���ȶ��в�֧���������
(2) ��ʹ֧�֣����ɾ��ĳ�ڵ���ƻ��˶ѵĽṹ����Ҫ���¸��¶ѽṹ��
���Զ��ڱ���Ϊdeleted��ʱ��ڵ㣬���ӳٵ���(1)��ʱ �� (2)��ǰ��Ľڵ㶼��ɾ��ʱ�����Żᱻɾ����
һ���㱻��Ϊdeleted,����ٻ���TIMER_TIME_OUTʱ���ɾ����
�������������ô���
(1) ��һ���ô��ǲ���Ҫ�������ȶ��У�ʡʱ��
(2) �ڶ����ô��Ǹ���ʱʱ��һ�����̵�ʱ�䣬�����趨�ĳ�ʱʱ����ɾ��������(������һ����ʱʱ�������ɾ��)����������������ڳ�ʱ�����һ����������һ�γ����ˣ�
�Ͳ�������������requestData�ڵ��ˣ��������Լ����ظ�����ǰ���requestData��������һ��delete��һ��new��ʱ�䡣
*/

void handle_expired_event()
{
    //ֻ�����ϵģ������ϵĹ����˾͵��������û���ھ��˳�ѭ��
    pthread_mutex_lock(&qlock);
    while (!myTimerQueue.empty())
    {
        mytimer* ptimer_now = myTimerQueue.top();
        //����Ƿ��Ѿ�����Ϊ����
        if (ptimer_now->isDeleted())
        {
            myTimerQueue.pop();
            delete ptimer_now;
        }
        //���֮ǰû���ڣ�����Ƿ����
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
    //��SIGPIPE����źŴ��������Ϊ��һ�����ιرյĹܵ���socket������д���ݻὫ����������������յ�
    //���ź�ʱ���������
    handle_for_sigpipe();      
    //����һ���ļ�������
    int epoll_fd = epoll_init();
   //�������ʧ�ܷ���
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
    //��listen_fd����Ϊ��������������Ƿ�ɹ�
    if (setSocketNonBlocking(listen_fd) < 0)
    {
        perror("set socket non block failed");
        return 1;
    }
    //��epoll�¼��������£�epollin�����¼�����EPOLLET����Ե������
    __uint32_t event = EPOLLIN | EPOLLET;
    requestData* req = new requestData();
    req->setFd(listen_fd);
    epoll_add(epoll_fd, listen_fd, static_cast<void*>(req), event);
    while (true)
    {
        //�ȴ��¼��Ĳ���
        int events_num = my_epoll_wait(epoll_fd, events, MAXEVENTS, -1);
        //printf("%zu\n", myTimerQueue.size());        
        if (events_num == 0)
            continue;
        printf("%d\n", events_num);
        //printf("%zu\n", myTimerQueue.size());    
        // else
        //     cout << "one connection has come!" << endl;
        // ����events���飬���ݼ������༰���������ͷַ�����
        handle_events(epoll_fd, listen_fd, events, events_num, PATH, threadpool);

        handle_expired_event();
    }
    return 0;
}