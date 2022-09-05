#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users; //用于存储从数据库查询到的所有用户、密码结果集

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
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

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改在该文件描述符上监听的事件
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
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

//循环从监听的socket上读取客户数据进入读缓冲区，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //strpbrk在源字符串（s1）中找出最先含有搜索字符串（s2）中任一字符的位置并返回，若找不到则返回空指针
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0'; //将该位置改为\0，用于将前面数据取出，已读取的数据不再会匹配到\t
    char *method = text; //取出数据，并通过与GET和POST比较，以确定请求方式
    if (strcasecmp(method, "GET") == 0) //strcasecmp用于比较字符串，第三个int型参数可以限定比较长度，相同返回0    
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;    //是否启用POST
    }
    else
        return BAD_REQUEST;

    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找继续跳过空格和\t字符，指向请求资源的第一个字符
    //strspn:检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    //即m_url跳过匹配的" \t"片段,保持在统一资源标识符开头
    m_url += strspn(m_url, " \t");   
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");  //m_version同理有，来到http协议版本的开头，并比较协议版本格式
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    //通常的访问资源都跟在单独的 / 后面
    //这里主要是有些报文的请求资源中会带有 http:// 或者 https://
    //这里需要对这两种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');  //m_url移动到访问资源前面的 /, strchr返回s1中字符c第一次出现的位置
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');  
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");   //strcat将字符串s2接续到s1的结尾
    m_check_state = CHECK_STATE_HEADER;   //请求行处理完毕，主状态机状态转移为解析请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0') //检查是否为空行，因为如果是空行，原本的回车符或换行符，已被parse_line()覆写为/0
    {
        if (m_content_length != 0)  //请求体为空，则是GET请求，不为空则为POST请求
        {
            m_check_state = CHECK_STATE_CONTENT;  //主状态机转化状态为解析请求体
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)  //字段是Connection
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)       //连接状态
        {
            m_linger = true;    //连接活跃则将m_linger设为true
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)  //字段是Content-length
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);   //取出主体长度
    }
    else if (strncasecmp(text, "Host:", 5) == 0)  //字段是Host
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;  //取出hos/*  */t内容
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx)) //此时请求体之前的数据已被checked
    {
        text[m_content_length] = '\0';  //如果已完整读入，则将最后一位覆写为/0表示已读
        m_string = text;  //存储POST请求体最后的用户名和密码字符串
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() //主状态机处理请求报文
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    //此处的循环逻辑，若请求报文的每一行末尾都是回车符 + 换行符，则只需要(line_status = parse_line()) == LINE_OK）
    //但为了避免登录和注册操作过程中用户名和密码直接暴露在url中，将其封装在请求体的末尾，因此请求体最后一行不是以回车符 + 换行符结尾
    //读请求体的非结尾行时，满足m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK
    //读完请求体结尾行，line_status != LINE_OK，以此退出循环
    //POST和GET的区别就是有无请求体，因此CHECK_STATE_CONTENT状态是特意为POST设计的
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();   //指向未处理数据的开始
        m_start_line = m_checked_idx;    //parse_line()更新了m_checked_idx
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);  //请求行
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);       //头部字段
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);       //请求体
            if (ret == GET_REQUEST)
                return do_request();

            //每读完一行请求体，都将从状态机设为LINE_OPEN
            //在解析请求体非结尾行时，从状态机能够通过parse_line()重新回到LINE_OK进入循环
            //在解析请求体结尾行时，从状态机保持LINE_OPEN状态，从而退出循环
            line_status = LINE_OPEN;  
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
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);  //记录网站根目录长度
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/'); //获取m_url中/的位置

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))  //2为登录，3为注册
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2); //把请求资源开头的标志位去掉，保留真正的请求资源名
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); //m_real_file开头是网站根目录，在后面接上请求资源名
        free(m_url_real); //释放m_url_real

        //将用户名和密码提取出来
        //user=123&passwd=123，放在请求体的最后
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
            char *sql_insert = (char *)malloc(sizeof(char) * 200);  //sql_insert用于存储sql插入语句
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end()) //如果是注册，先检测数据库中是否有重名的
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);  //没有重名的，进行增加数据, 要上锁
                users.insert(pair<string, string>(name, password)); 
                m_lock.unlock();

                if (!res)  
                    strcpy(m_url, "/log.html"); //根据结果的不同，给m_url赋不同的资源名
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

    if (*(p + 1) == '0') //只需补齐m_real_file
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));   

        free(m_url_real);
    }
    else if (*(p + 1) == '1')  //只需补齐m_real_file
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')  //只需补齐m_real_file
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')  //只需补齐m_real_file
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')  //只需补齐m_real_file
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0) //判断文件资源是否存在
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH)) //判断文件权限
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode)) //判断文件是否为目录
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0); //将文件内容映射到内存中
    close(fd);
    return FILE_REQUEST; //请求资源文件正常，跳转process_write
}
void http_conn::unmap() //删除特定区域的映射
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0) //要发送的响应报文为空
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //修改文件描述符上的监听事件为读事件
        init();  //初始化接收新连接
        return true;
    }

    while (1) //循环writev，需要不断重置iovec数组
    {
        temp = writev(m_sockfd, m_iv, m_iv_count); //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端

        if (temp < 0) //发送失败
        {
            //文件描述符是非阻塞的，因此writev发送数据后会立即返回而不去确认socket缓冲区是否满了
            //满了的话，则会产生EAGAIN错误，字面意思是再试一次
            //errno包含在errno.h中
            if (errno == EAGAIN) 
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); 
                return true;
            }
            unmap(); //发送完响应报文，自然要删除映射
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len) //状态行、响应头、空行已发送完毕
        {
            m_iv[0].iov_len = 0; 
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx); //记录请求资源文件未发送的数据的起始位置
            m_iv[1].iov_len = bytes_to_send; //记录请求资源文件未发送的数据长度
        }
        else  //状态行、响应头、空行未发送完毕，则更新m_iv[0]
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;  
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) //全部发送完成
        {
            unmap(); //删除映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //重新开始监听读事件

            if (m_linger) //连接正常
            {
                init();  //初始化新接受的连接
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表
    va_list arg_list;
    //初始化arg_list变量，format为最后一个传入的已知参数
    va_start(arg_list, format);
    //将可变参数以format格式写入写缓冲区，并返回写入数据长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) //超出缓冲区剩余长度
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); //添加状态行
}
bool http_conn::add_headers(int content_len)  //添加响应头（包括响应体长度、连接状态、空行）
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len) //添加响应体长度
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type() //添加响应体类型
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger() //添加连接状态
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() //添加空行
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content) //添加响应体
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret) //ret为do_request()返回值
{
    switch (ret)
    {
    case INTERNAL_ERROR: //服务器错误
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:   //请求报文有语法错误或请求资源为目录
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST: //请求资源无权访问
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: //请求资源可以正常访问
    {
        add_status_line(200, ok_200_title); //把请求行写入缓冲区
        if (m_file_stat.st_size != 0)  //m_file_stat.st_size为请求资源文件长度
        {
            add_headers(m_file_stat.st_size);  //把请求头写入缓冲区
            m_iv[0].iov_base = m_write_buf;    //存储响应报文的写缓冲区
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address; //请求资源文件映射到的内存地址
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //要发送的总字节数为写缓冲区内数据 + 请求资源文件长度
            bytes_to_send = m_write_idx + m_file_stat.st_size; 
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>"; //请求体为空
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;  //请求资源无法正常访问，不再编写响应体
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) //请求不完整，需要继续接收请求数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode); //修改文件描述符上的监听事件为读事件
        return;
    }
    bool write_ret = process_write(read_ret); //完成响应报文并存入内存
    if (!write_ret)
    {
        close_conn(); //响应报文失败，则关闭连接
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode); //修改文件描述符上的监听事件为写事件
}
