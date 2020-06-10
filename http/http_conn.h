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
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    //文件名的最大长度
    static const int FILENAME_LEN = 200;
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //http请求方法
    enum METHOD
    {
        GET = 0,//申请获取资源,不对服务器产生任何其他影响
        POST,//客户端向服务器提交数据
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机可能状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,//正在分析请求行
        CHECK_STATE_HEADER,//正在分析头部字段
        CHECK_STATE_CONTENT//正在处理消息体
    };
    //服务器处理http请求的结果
    enum HTTP_CODE
    {
        NO_REQUEST,//请求不完整,需要继续读取客户数据
        GET_REQUEST,//获得了一个完整的客户请求
        BAD_REQUEST,//有语法错误
        NO_RESOURCE,
        FORBIDDEN_REQUEST,//客户对资源没有足够的访问权限
        FILE_REQUEST,//文件存在
        INTERNAL_ERROR,//服务器内部错误
        CLOSED_CONNECTION//客户端已经关闭连接
    };
    //从状态机可能状态
    enum LINE_STATUS
    {
        LINE_OK = 0,//读取一个完整行
        LINE_BAD,//行出错
        LINE_OPEN//行数据不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化新接受的连接
    void init(int sockfd, const sockaddr_in &addr);
    //关闭连接
    void close_conn(bool real_close = true);
    //处理客户请求
    void process();
    //非阻塞读操作
    bool read_once();
    //非阻塞写操作
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);
    void initresultFile(connection_pool *connPool);

private:
    //初始化连接
    void init();
    //解析http请求
    HTTP_CODE process_read();
    //填充http应答
    bool process_write(HTTP_CODE ret);

    //下面这组函数被process_read调用以分析http请求
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();

    //下面这组函数被process_write调用以填充http请求
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    //所有socket上的事件都被注册到同一个epoll内核时间表中,所以将epoll文件描述符设置为静态
    static int m_epollfd;
    //统计用户数量
    static int m_user_count;
    MYSQL *mysql;

private:
    //该http连接的socket和对方socket的地址
    int m_sockfd;
    sockaddr_in m_address;

    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //标示读缓冲中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    //当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;
    //当前正在解析的行的起始位置
    int m_start_line;
    //写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    //写缓冲区中待发送的字节数
    int m_write_idx;

    //主状态机当前所处的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //客户请求的目标文件的完整路径,其内容等于doc_root+m_url,doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    //客户请求的目标文件和文件名
    char *m_url;
    //http协议版本号
    char *m_version;
    //主机名
    char *m_host;
    //http请求的消息体的长度
    int m_content_length;
    //http请求是否要求保持连接
    bool m_linger;

    //客户请求的目标文件被mmap到内存中的起始位置
    char *m_file_address;
    //目标文件的状态,通过它我们可以判断文件是否存在、是否为目录、是否可读,并获取文件大小等信息
    struct stat m_file_stat;
    //将采用writev来执行写操作,m_iv_count表示被写到内存块的数量
    struct iovec m_iv[2];
    int m_iv_count;

    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;//需要发送的字节数
    int bytes_have_send;//已发送字节数
};

#endif
