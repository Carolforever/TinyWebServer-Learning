#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head; //析构函数删除整条定时器链表，即定时器容器
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer) //链表插入定时器
{
    if (!timer)
    {
        return;
    }
    if (!head) //定时器容器为空，直接放入容器
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire) //新定时器的超时时间比链表头还短，则成为新的头节点
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head); //调用add_timer在链表中寻找合适位置插入
}
void sort_timer_lst::adjust_timer(util_timer *timer) //调整定时器在链表中的位置
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    //被调整的定时器在链表尾部
    //or 定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    //被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
void sort_timer_lst::del_timer(util_timer *timer) //从链表中删除定时器
{
    if (!timer)
    {
        return;
    }
    //链表中只有一个定时器
    if ((timer == head) && (timer == tail)) 
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //该定时器在链表头
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //该定时器在链表尾
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    //该定时器在链表中
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
void sort_timer_lst::tick() //定时任务处理函数
{
    if (!head)
    {
        return;
    }
    
    time_t cur = time(NULL); //取得当前时间
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire) //当前时间早于遍历到的定时器的超时时间，则停止处理
        {
            break;
        }
        tmp->cb_func(tmp->user_data); //当前定时器到期，则调用回调函数，执行定时事件
        //处理完当前定时事件，则删除该定时器
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
//加入定时器，不考虑加入的定时器为头节点的情况
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    //利用前后指针进行插入
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    //如果链表中所有定时器的超时时间都短于该定时器，该定时器成为尾部
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
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
    m_timer_lst.tick(); //处理任务
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
