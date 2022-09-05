#include "webserver.h"

WebServer::WebServer()
{
    //http_conn类对象，用于存储用户连接
    users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    //getcwd用于将当前的工作目录存入参数1指定缓冲区中，参数2为缓冲区大小
    getcwd(server_path, 200); 
    //初始化根目录的绝对地址
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    users_timer = new client_data[MAX_FD]; //定时器数量也和文件描述符数量上限有关
}

WebServer::~WebServer() //服务器资源释放
{
    close(m_epollfd); //关闭epoll
    close(m_listenfd); //停止监听
    //关闭用于给主循环发送信号，通知其处理定时器的管道
    close(m_pipefd[1]); 
    close(m_pipefd[0]);
    delete[] users; //删除http_conn类对象
    delete[] users_timer; //删除定时器
    delete m_pool; //删除线程池
}

//构造函数初始化
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, 
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

//设置epoll触发模式(考虑监听和连接事件是否开启ET模式)
void WebServer::trig_mode()
{
    //LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    //LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    //ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    //ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write) //异步写日志
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else  //同步写日志
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}
//监听相关
void WebServer::eventListen()
{
    //创建socket
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭TCP连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address; //ipv4协议下的socket地址
    bzero(&address, sizeof(address)); //将socket地址内存空间置0
    address.sin_family = AF_INET; //设置地址的地址族
    //INADDR_ANY表示监听0.0.0.0地址
    //socket只绑定端口，不绑定本主机的某个特定ip，让路由表决定传到哪个ip(0.0.0.0地址表示所有地址、不确定地址、任意地址)
    //IP地址要注意转化为长整型网络字节序
    address.sin_addr.s_addr = htonl(INADDR_ANY); 
    address.sin_port = htons(m_port); //设置端口号，注意转化为短整型网络字节序

    //能够让m_listenfd即使处于TIME_WAIT的状态，与之绑定的socket地址也能立即被重用
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    //socket命名，将文件描述符与socket地址绑定
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    //监听socket, 设置内核监听队列长度为5
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT); //初始化alarm函数触发的时间间隔

    //epoll_wait会将就绪事件从内核事件表中取出放入events数组中
    epoll_event events[MAX_EVENT_NUMBER];

    //epoll创建内核事件表
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    //往内核事件表中注册监听事件(实则为读事件，不开启oneshot)
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;
    
    //在管道中创建一对互相连接的匿名socket,从任意一端写入,都能从另一端读
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]); //第二个socket设置非阻塞用于写入
    utils.addfd(m_epollfd, m_pipefd[0], false, 0); //epoll监听第一个socket上的读事件

    //分别设置三种信号的处理方式
    utils.addsig(SIGPIPE, SIG_IGN); //往读端被关闭的管道或者socket连接写数据，则忽略信号
    utils.addsig(SIGALRM, utils.sig_handler, false); //由alarm超时引起
    utils.addsig(SIGTERM, utils.sig_handler, false); //term信号，终止进程

    alarm(TIMESLOT); //初始化alarm

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}
//初始化定时器
//第二个参数为被接受连接的远端socket地址
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    //初始化用户连接，用户连接数+1
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func; //将定时器类中的函数指针指向回调函数cb_func
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT; //定时器向后延时三个单位
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer); //将定时器插入链表
}

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

//删除定时器
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]); //调用回调函数从内核事件表中删除事件
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer); //从链表中删除定时器
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//接受连接并分配定时器
bool WebServer::dealclinetdata()
{
    struct sockaddr_in client_address; //被接受连接的远端socket地址
    socklen_t client_addrlength = sizeof(client_address); //client_address地址长度,为socklen_t类型
    if (0 == m_LISTENTrigmode) //监听为LT模式
    {
        //从内核监听队列中取出一个连接，并用connfd唯一地表示这个连接
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0) //accept连接失败
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) //连接数量达到上限
        {
            utils.show_error(connfd, "Internal server busy"); //将内部错误send到connfd的缓冲区
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address); //为该连接初始化一个定时器并放入链表中
    }

    else //监听为ET模式，由于只会通知一次，所以需要循环accept
    {
        //不断地尝试从内核监听队列中取出一个连接，并用connfd唯一地表示这个连接,直到连接失败或者连接数量达到上限
        while (1) 
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0) //accept连接失败
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) //连接数量达到上限
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address); //为该连接初始化一个定时器并放入链表中
        }
        return false;
    }
    return true;
}
//信号处理
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0); //从第一个管道读入信号并放入signals缓冲区，返回读到的数量
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM: //alarm超时触发
            {
                timeout = true;
                break;
            }
            case SIGTERM: //终止信号
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}
//读事件处理
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer; //取出读事件的定时器

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer); //延时定时器并调整位置
        }

        //若监测到读事件，将该事件放入请求队列，等待线程进行I/O操作
        m_pool->append(users + sockfd, 0);

        while (true) //循环等待读事件被处理
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    //读事件read_once失败，则删除定时器
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                //成功处理，则重置improv,并结束循环
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once()) //由主线程进行读取数据
        {
            //inet_ntoa将网络地址转化为'.'间隔的字符串
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列，等待线程处理请求报文
            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer); //延时定时器并调整位置
            }
        }
        else
        {
            deal_timer(timer, sockfd); //主线程读取数据失败，删除定时器
        }
    }
}
//写事件处理
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write()) //主线程完成写数据，无需再进入请求队列分配线程
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}
//事件回环(即服务器主线程)
void WebServer::eventLoop()
{
    bool timeout = false; //信号处理后alarm是否超时
    bool stop_server = false; //是否关闭服务器

    while (!stop_server)
    {
        //number为就绪事件数量
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //EINTR为系统中断信号
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd) //判断该就绪事件是否来自于监听socket
            {
                bool flag = dealclinetdata(); //接受连接并分配定时器
                if (false == flag)
                    continue;
            }
            //对端被关闭或文件描述符被挂断/出现错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            //该就绪事件为管道的读入信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) //就绪事件为读事件
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT) //就绪事件为写事件
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout) //超时
        {
            utils.timer_handler(); //处理超时定时器，从内核事件表删除不活跃连接的文件描述符

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}