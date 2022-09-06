#include "lst_timer.h"
#include "../http/http_conn.h"

timer_wheel::timer_wheel() //初始化时间轮，将各定时器链表置为空，当前槽为0
{
    cur_slot = 0;
    for(int i = 0; i < N; i++){
        slots[i] = NULL;
    }
}
timer_wheel::~timer_wheel() //删除所有定时器链表
{
   for(int i = 0; i < N; i++){
        util_timer* tmp = slots[i];
        while(tmp){
            slots[i] = tmp->next;
            delete tmp;
            tmp = slots[i];
        }
   }
}
//将定时器插入时间轮
void timer_wheel::add_timer(util_timer *timer) 
{
    if (!timer)
    {
        return;
    }
    int timeout = timer->expire;
    if(timeout < 0){
        return;
    }
    int ticks = 0; //计算定时器需要滴答几次后触发
    if(timeout < SI){
        ticks = 1;
    }
    else{
        ticks = timeout / SI;
    }
    int rotation = ticks / N; //计算定时器需要时间轮转动几圈后触发
    int ts = (cur_slot + (ticks % N)) % N; //计算定时器所在的槽
    timer->rotation = rotation;
    timer->time_slot = ts;
    //将定时器插入对应槽的定时器链表
    if(!slots[ts]){ //若该链表为空
        slots[ts] = timer;
    }
    else{ //不为空，则将timer设为头节点
        timer->next = slots[ts];
        slots[ts]->prev = timer;
        slots[ts] = timer;
    }
}
void timer_wheel::adjust_timer(util_timer *timer) //调整定时器位置
{
    if (!timer)
    {
        return;
    }
    int ts = timer->time_slot; //该定时器位于槽ts
    if(timer == slots[ts]){ //该定时器是ts槽定时器链表的头节点
        slots[ts] = slots[ts]->next;
        if(slots[ts]){
            slots[ts]->prev = NULL;
        }
        add_timer(timer);
    }
    else{ //该定时器位于ts槽定时器链表中
        timer->prev->next = timer->next;
        if(timer->next){
            timer->next->prev = timer->prev;
        }
        add_timer(timer);
    }
}
void timer_wheel::del_timer(util_timer *timer) //从链表中删除定时器
{
    if (!timer)
    {
        return;
    }
    int ts = timer->time_slot;
    if(timer = slots[ts]){ //该定时器是ts槽定时器链表的头节点
        slots[ts] = slots[ts]->next;
        if(slots[ts]){
            slots[ts]->prev = NULL;
        }
        delete timer;
    }
    else{ //该定时器位于ts槽定时器链表中
        timer->prev->next = timer->next;
        if(timer->next){
            timer->next->prev = timer->prev;
        }
        delete timer;
    }
}
void timer_wheel::tick() //定时任务处理函数
{
    util_timer* tmp = slots[cur_slot]; //取出时间轮当前槽定时器链表的头节点
    while(tmp){
        if(tmp->rotation > 0){ //定时器不是这一圈未超时
            tmp->rotation--;
            tmp = tmp->next;
        }
        else{ //定时器超时，执行定时任务并删除定时器
            tmp->cb_func(tmp->user_data);
            if(tmp == slots[cur_slot]){
                slots[cur_slot] = tmp->next;
                if(slots[cur_slot]){
                    slots[cur_slot]->prev = NULL;
                }
                delete tmp;
                tmp = slots[cur_slot];
            }
            else{
                tmp->prev->next = tmp->next;
                if(tmp->next){
                    tmp->next->prev = tmp->prev;
                }
                util_timer* tmp2 = tmp->next;
                delete tmp;
                tmp = tmp2;
            }
        }
    }
    cur_slot = ++cur_slot % N; //转动时间轮，即滴答一次
}

void Utils::init(int timeslot) //初始化alarm函数触发的时间间隔
{
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数
//信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    //将信号值从管道写端写入，传输字符类型，而非整型
    //将信号值存储的内存地址作为发送缓冲区，发送字节数为1
    send(u_pipefd[1], (char *)&msg, 1, 0); 
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //新建sigaction结构体
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //函数指针指向信号处理函数
    sa.sa_handler = handler; 
    if (restart)
        sa.sa_flags |= SA_RESTART; //让被信号打断的系统调用自动重新发起
    //将所有信号添加到信号处理函数执行期间需要屏蔽的信号集中
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1); //对信号sig设置新的处理方式，参数类型为sigaction结构体指针
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_wheel.tick(); //处理任务
    alarm(m_TIMESLOT); //重新设定alarm
}

void Utils::show_error(int connfd, const char *info) 
{
    //将错误信息写入文件描述符，并关闭连接，解除占用
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
//定时器回调函数:从内核事件表删除事件，关闭文件描述符，释放连接资源
void cb_func(client_data *user_data) 
{
    //从u_epollfd中删除sockfd
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    //关闭连接，解除占用
    close(user_data->sockfd);
    //连接数-1
    http_conn::m_user_count--;
}
