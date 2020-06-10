#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//同步校验
#define SYNSQL

#define ET       //边缘触发非阻塞
//#define LT         //水平触发阻塞

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

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/zhanghao/TinyWebServer/root";

int setnonblocking(int fd){
    int old_option=fcntl(fd,F_GETFL);
    int new_option=old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

void addfd(int epollfd,int fd,bool one_shot){
    epoll_event event;
    event.data.fd=fd;

#ifdef ET
    //EPOLLRDHUP表示TCP连接被对方关闭
    event.events=EPOLLIN|EPOLLET|EPOLLRDHUP;
#endif

#ifdef LT
    event.events=EPOLLIN|EPOLLRDHUP;
#endif

    if(one_shot){
        //指定一个socket任一时刻只能被一个线程处理
        event.events|=EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//注册了EPOLLONESHOT事件的socket在被某个线程处理完毕后应当重置,具体见书本P157
void modfd(int epollfd,int fd,int ev){
    epoll_event event;
    event.data.fd=fd;
#ifdef ET
    event.events=ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
#endif

#ifdef LT
    event.events=ev|EPOLLONESHOT|EPOLLRDHUP;
#endif
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_conn){
    if(real_conn&&(m_sockfd!=-1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        --m_user_count;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in& addr){
    m_sockfd=sockfd;
    m_address=addr;
    addfd(m_epollfd,sockfd,true);
    ++m_user_count;

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
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
httpconn::LINE_STATUS httpconn::parse_line(){
    char temp;
    for(;m_check_idx<m_read_idx;++m_checked_idx){
        temp=m_read_buf[m_checked_idx];
        if(temp=='\r'){
            if((m_checked_idx+1)==m_read_idx) return LINE_OPEN;
            else if(m_read_buf[m_checked_idx+1]=='\n'){
                m_read_buf[m_checked_idx++]=='\0';
                m_read_buf[m_checked_idx++]=='\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if(temp=='\n'){
            if((m_checked_idx>1)&&(m_read_buf[m_checked_idx-1]=='\r')){
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
bool httpconn::read_once(){
    if(m_read_idx>=READ_BUFFER_SIZE){
        return false;
    }

    int bytes_read=0;
    while(true){
        //从套接字接收数据，存储在m_read_buf缓冲区
        bytes_read=recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK) break;

            return false;
        }
        else if(bytes_read==0) return false;

        m_read_idx+=bytes_read;
    }
    return true;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text){
    //extern char *strpbrk(char *str1, char *str2)
    //比较字符串str1和str2中是否有相同的字符，如果有，则返回该字符在str1中的位置的指针。
    m_url=strpbrk(text,"\t");
    if(!m_url) return BAD_REQUEST;
    *m_url++='\0';

    char* method=text;
    //strcasecmp用忽略大小写比较字符串，通过strcasecmp函数可以指定每个字符串用于比较的字符数
    if(strcasecmp(method,"GET")==0){
        m_method=GET;
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else{
        return BAD_REQUEST;
    }

    m_url+=strspn(m_url,"\t");
    m_version=strpbrk(m_url,"\t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++='\0';
    m_version+=strspn(m_version,"\t");
    if(strcasecmp(m_version,"HTTP/1.1")!=0){
        return BAD_REQUEST;
    }

    //strncasecmp()用来比较参数s1和s2字符串前n个字符，比较时会自动忽略大小写的差异。
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        //strchr函数功能为在一个串中查找给定字符的第一个匹配之处。
        m_url = strchr(m_url, '/');
    }

    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1){
        //将两个char类型连接,中间无空格
        strcat(m_url, "judge.html");
    }
    m_check_state=CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    //遇到空行,表示头部字段解析完毕
    if(text[0]=='\0'){
        //如果http请求有消息体,则还需要读取m_content_length字节的消息体
        if(m_content_length!=0){
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }

        return GET_REQUEST;
    }
    else if(strncasecmp(text,"Connection:",11)==0){
        text+=11;
        text+=strspn(text,"\t");
        if(strcasecmp(text,"keep-alive")==0){
            m_linger=true;
        }
    }
    else if(strncasecmp(text,"Content-Length:",15)==0){
        text+=15;
        text+=strspn(text,"\t");
        m_content_length=atol(text);
    }
    else if(strncasecmp(text,"Host:",5)==0){
        text+=5;
        text+=strspn(text,"\t");
        m_host=text;
    }
    else{
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//这里并不解析http请求的消息体,只是判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    if(m_read_idx>=(m_content_length+m_checked_idx)){
        text[m_content_length]='\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }

    return NO_REQUEST;
}

//主状态机,用于从读缓冲区中取出所有完整的行
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;
    char* text=0;

    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))
        ||((line_status=parse_line())==LINE_OK))
    {
        text = get_line();//行在读缓冲区中起始位置
        m_start_line = m_checked_idx;//记录下一行的起始位置
        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        switch(m_check_state){
            //分析请求行
            case CHECK_STATE_REQUESTLINE:
            {
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            //分析头部字段
            case CHECK_STATE_HEADER:
            {
                ret=parse_headers(text);
                if (ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }                
                else if (ret == GET_REQUEST)
                {   
                    return do_request();
                }
                break;
            }
            //处理消息体
            case CHECK_STATE_CONTENT: 
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST){
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

//当得到一个完整、正确的http请求时,分析目标文件的属性.
//如果目标文件存在、对所有用户可读,则使用mmap将其映射到内存地址m_file_address处,并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request(){
    strcpy(m_real_file,doc_root);
    int len=strlen(doc_root);

    //char *strrchr(const char *str, int c) 
    //在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置。
    const char *p = strrchr(m_url, '/');

    if(cgi==1&& (*(p + 1) == '2' || *(p + 1) == '3')){
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);
//同步线程登录校验
#ifdef SYNSQL
        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

                pthread_mutex_t lock;
        pthread_mutex_init(&lock, NULL);

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                pthread_mutex_lock(&lock);
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                pthread_mutex_unlock(&lock);

                if (!res)
                    strcpy(m_url, "/log.html");
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
#endif       
    }

        //跳转注册界面
    if (*(p + 1) == '0'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //跳转登录界面
    else if (*(p + 1) == '1'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //跳转图片请求
    else if (*(p + 1) == '5'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //跳转视频请求
    else if (*(p + 1) == '6'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //跳转关注页面
    else if (*(p + 1) == '7'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else{
        //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE;
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST;

    int fd=open(m_real_file,O_RDONLY);
    m_file_address=mmap(0,m_file_stat.st_size,PROT_READ, MAP_PRIVATE, fd, 0);
    close fd;
    return FILE_REQUEST;
}

void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}

//写http响应,服务器子线程调用process_write完成响应报文，随后注册epollout事件。
//服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
bool http_conn::write(){
    int temp=0;
    int newadd=0;

    //若发送数据长度为0,表示响应报文为空，一般不会出现这种情况
    if(bytes_to_send==0){
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init;
        return true;
    }

    while(true){
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp=writev(m_sockfd,m_iv,m_iv_count);

        if(temp>0){
            //更新已发送字节
            bytes_have_send+=temp;
            //偏移文件iovec的指针
            newadd=bytes_have_send-m_write_idx;
        }
        if(temp<=-1){
            //如果TCP写缓存没有空间
            if(errno=EAGAIN){
                //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
                if(bytes_have_send>=m_iv[0].iov_len){
                    m_iv[0].iov_len=0;
                    m_iv[1].iov_base=m_file_address+newadd;
                    m_iv[1].iov_len=bytes_to_send;
                }
                //继续发送第一个iovec头部信息的数据
                else{
                    m_iv[0].iov_base=m_write_buf+bytes_to_send;
                    m_iv[0].iov_len=m_iv[0].iov_len-bytes_have_send;
                }
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }
        bytes_to_send-=temp;

        //判断数据是否已发完
        if(bytes_to_send<=0){
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN);

            //浏览器请求长连接
            if(m_linger){
                init();
                return true;
            }
            else{
                return false;
            }
        }
    }
}

bool http_conn::add_response(cosnt char* format,...){
    if(m_write_idx>=WRITE_BUFFER_SIZE){
        return false;
    }

    va_list arg_list;
    va_start(arg_list,format);
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len=vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx)){
        return false;
    }

    //更新m_write_idx位置
    m_write_idx+=len;
    va_end(arg_list);
    return 
}

//添加状态行
bool http_conn::add_status_line(int status,const char* title){
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

//添加消息报头，具体为添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

//添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length:%d\r\n",content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n","text/html");
}

//添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger(){
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}

//添加空行
bool http_conn::add_blank_line(){
    return add_response("%s","\r\n");
}

//添加文本content
bool http_conn::add_content(const char* content){
    return add_response("%s",content);
}

//根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文。
bool http_conn::process_write(HTTP_CODE ret){
    switch(ret){
        case INTERNAL_ERROR:
        {
            add_status_line(500,error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form)){
                return false;
            }
            break;
        }
        case BAD_REQUEST:
        {
            add_status_line(400,error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_conrent(error_400_form)){
                return false;
            }
            break;
        }
        case NO_REQUEST:
        {
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_conrent(error_404_form)){
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_conrent(error_403_form)){
                return false;
            }
            break;
        }
        //请求的文件存在，通过io向量机制iovec，声明两个iovec，第一个指向m_write_buf，第二个指向mmap的地址m_file_address
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            if(m_file_stat.st_size!=0){
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base=m_write_buf;
                m_iv[0].iov_len=m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
                m_iv[1].iov_base=m_file_address;
                m_iv[1].iov_len=m_file_stat.st_size;
                m_iv_count=2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else{
                const char *ok_string="<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string)){
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }

    //请求出错，这时候只申请一个iovec，指向m_write_buf。
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}