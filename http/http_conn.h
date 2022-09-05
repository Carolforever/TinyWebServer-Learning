#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200; //请求资源名(去掉了开头的/) + 网站根目录长度
    static const int READ_BUFFER_SIZE = 2048; //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024; //写缓冲区大小
    enum METHOD          //http请求方法
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE     //主状态机状态
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE       //报文解析结果
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS      //从状态机状态
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化连接，即往内核事件表中注册socket的fd，并初始化接受新连接
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true); //关闭连接，即从内核事件表中删除socket的fd
    void process(); //处理请求报文，并完成响应报文，存入内存
    bool read_once(); //循环从监听的socket上读取客户数据进入读缓冲区，直到无数据可读或对方关闭连接，区分LT和ET模式
    bool write(); //将内存中的请求报文发送到socket缓冲区
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool); //将数据库中所有的用户名和密码存入map
    //记录Reactor模式下读写任务的处理情况
    int timer_flag; 
    int improv;


private:
    void init(); //初始化接受新连接
    LINE_STATUS parse_line(); //从状态机以行为单位解析请求报文
    HTTP_CODE process_read(); //主状态机解析http请求，若获得完整请求，调用do_request
    bool process_write(HTTP_CODE ret); //将请求报文写入写缓冲区，并利用iovec数组管理写缓冲区和资源文件的映射内存区域
    HTTP_CODE parse_request_line(char *text); //解析请求行
    HTTP_CODE parse_headers(char *text); //解析请求头
    HTTP_CODE parse_content(char *text); //判断请求是否被完整读入，并用m_string取出请求体结尾的用户名密码字符串

    //解析完请求后，通过m_url判断请求类型，再通过修改m_url并组合网站根目录成为资源文件的完整地址
    //其中登录和注册的操作需要从m_string提取用户名和密码，注册还需要对数据库进行操作
    //最后利用stat获取文件属性，open文件，并利用mmap将文件内容映射进内存
    HTTP_CODE do_request();

    char *get_line() { return m_read_buf + m_start_line; }; //获取当前读入数据位置
    void unmap(); //删除资源文件与内存的映射
    bool add_response(const char *format, ...); //利用可变参数，为后续将响应报文各部分写入写缓冲区提供通用函数
    bool add_content(const char *content); //将响应体写入写缓冲区
    bool add_status_line(int status, const char *title); //将状态行写入写缓冲区
    bool add_headers(int content_length); //将响应头写入写缓冲区（包括响应体长度、连接状态、空行）
    bool add_content_type(); //将响应体类型写入写缓冲区
    bool add_content_length(int content_length); //将响应体长度写入写缓冲区
    bool add_linger(); //将连接状态写入写缓冲区
    bool add_blank_line(); //将空行写入写缓冲区

public:
    static int m_epollfd; //epoll标识
    static int m_user_count; //用户连接数
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd; //当前的连接socket
    sockaddr_in m_address; //当前的连接socket地址
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;  //已读的内容最后一位
    int m_checked_idx; //已解析的内容最后一位
    int m_start_line; //已读字节数
    char m_write_buf[WRITE_BUFFER_SIZE]; //写缓冲区
    int m_write_idx; //写入的内容最后一位
    CHECK_STATE m_check_state; //主状态机状态
    METHOD m_method; //HTTP请求方法 
    char m_real_file[FILENAME_LEN]; //读缓冲区
    char *m_url; //统一资源标识，通常以/开头
    char *m_version; //HTTP版本
    char *m_host;  //host
    int m_content_length; //请求体长度
    bool m_linger; //连接状态
    char *m_file_address; //资源文件地址
    struct stat m_file_stat; //stat用于存储文件属性
    struct iovec m_iv[2]; //管理两块内存，存储响应报文的写缓冲区和请求资源文件的映射内存区域
    int m_iv_count; 
    int cgi;        //是否启用的POST
    char *m_string; //存储请求体最后一行的用户名和密码字符串：user=123&passwd=123
    int bytes_to_send; //要发送的字节数
    int bytes_have_send; //已发送的字节数
    char *doc_root; //网站根目录，文件夹内存放请求的资源和跳转的html文件

    map<string, string> m_users;
    int m_TRIGMode; //ET模式标志
    int m_close_log; //日志关闭标志

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
