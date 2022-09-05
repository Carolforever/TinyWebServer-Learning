#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();
    //初始化用户名、数据库等相关成员变量
    void init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool(); //创建线程池
    void sql_pool(); //初始化数据库连接池
    void log_write(); //初始化日志系统
    void trig_mode(); //设置epoll的触发模式
    void eventListen(); //开启epoll监听
    void eventLoop(); //事件回环（即服务器主线程）
    void timer(int connfd, struct sockaddr_in client_address); //初始化定时器
    void adjust_timer(util_timer *timer); //调整定时器
    void deal_timer(util_timer *timer, int sockfd); //删除定时器
    bool dealclinetdata(); //http 处理用户数据
    bool dealwithsignal(bool& timeout, bool& stop_server); //处理定时器信号
    void dealwithread(int sockfd); //处理客户连接上接收到的数据
    void dealwithwrite(int sockfd); //写操作

public:
    //基础
    int m_port; //端口号
    char *m_root; //根目录
    int m_log_write; //异步写日志标志
    int m_close_log; //日志关闭标志
    int m_actormodel; //事件处理模式

    int m_pipefd[2]; //管道,[0]用于读,[1]用于写
    int m_epollfd; //epoll标志
    http_conn *users; //大小与文件描述符上限相等

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num; //数据库连接池容量

    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num; //线程池容量

    //epoll_event相关
    //epoll_wait会将就绪事件从内核事件表中取出放入events数组中
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd; //监听socket
    int m_OPT_LINGER; //是否优雅关闭监听socket的连接
    int m_TRIGMode; //epoll触发模式，包括下面的监听和连接
    int m_LISTENTrigmode; //监听触发模式
    int m_CONNTrigmode; //连接触发模式

    //定时器相关
    client_data *users_timer; //用于存储初始化后的定时器
    Utils utils; //设置定时器的实例
};
#endif
