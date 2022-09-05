#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class util_timer; //前向声明

struct client_data //连接资源
{
    sockaddr_in address; //socket地址
    int sockfd; //文件描述符
    util_timer *timer;  //定时器类指针指向连接对应的定时器
};

class util_timer //定时器类，利用双向链表实现
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire; //超时时间
    //回调函数，从内核事件表删除事件，关闭文件描述符，释放连接资源
    //定义函数指针cb_func，使用时指向要使用的函数，该函数的参数为client_data*类型
    void (* cb_func)(client_data *); 
    client_data *user_data;  //连接资源
    util_timer *prev;  //前向定时器
    util_timer *next;  //后向定时器
};

class sort_timer_lst //定时器容器类，定时器容器用于管理多个定时器
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer); //添加定时器，内部调用私有成员add_timer
    void adjust_timer(util_timer *timer); //调整定时器，任务发生变化时，调整定时器在链表中的位置
    void del_timer(util_timer *timer); //删除定时器
    void tick(); //定时任务处理函数

private:
    //私有成员，被公有成员add_timer和adjust_time调用
    //主要用于调整链表内部结点
    void add_timer(util_timer *timer, util_timer *lst_head);
    //头尾结点
    util_timer *head;
    util_timer *tail;
};

class Utils //设置定时器
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    //使用管道通知主循环执行定时器链表的任务
    //逻辑顺序，设置信号后，触发时调用信号处理函数，信号处理函数通过管道将sig发送到主循环
    //主循环通过管道接收sig，得知有定时器超时，再调用定时器处理任务函数timer_handler()处理，并且再次设定ALARM信号触发，形成循环
    static int *u_pipefd; //管道，用于存储文件描述符
    sort_timer_lst m_timer_lst; //定时器容器
    static int u_epollfd; //epoll标识
    int m_TIMESLOT; //alarm函数触发的时间间隔
};

void cb_func(client_data *user_data); //定时器回调函数

#endif
