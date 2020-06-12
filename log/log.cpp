#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log(){
    m_count=0;
    m_is_async=false;
    pthread_mutex_init(m_mutex,NULL);
}

Log::~Log(){
    if(m_fp!=NULL){
        fclose(m_fp);
    }
    pthread_mutex_destroy(m_mutex);

    if(m_mutex!=NULL){
        delete m_mutex;
    }
}

bool Log::init(const char*file_name,int log_buf_size,int split_lines,int max_queue_size){
    //如果设置了max_queue_size,则设置为异步
    //因为异步需要设置阻塞队列的长度，同步不需要设置
    if(max_queue_size>=1){
        m_is_async=true;

        //创建并设置阻塞队列长度
        m_log_queue=new block_queue<string>(max_queue_size);
        pthread_t tid;

        //创建线程异步写日志
        pthread_create(&tid,NULL,flush_log_thread,NULL);
    }

    //输出内容的长度
    m_log_buf_size=log_buf_size;
    m_buf=new char[m_log_buf_size];
    memset(m_buf,'\0',sizeof(m_buf));
    
    m_split_lines=split_lines;

    time_t t=time(NULL);
    struct tm *sys_tm=localtime(&t);
    struct tm my_tm=*sys_tm;

    //从后往前找到第一个/的位置
    const char*p=strrchr(file_name,'/');
    char log_full_name[256]={0};

    //自定义日志名,若输入的文件名没有/，则直接将时间+文件名作为日志名
    if(p==NULL){
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else{
        //将/的位置向后移动一个位置，然后复制到logname中
        //p - file_name + 1是文件所在路径文件夹的长度
        //dirname相当于./
        strcpy(log_name,p+1);
        strncpy(dir_name,file_name,p-file_name+1);

        //后面的参数跟format有关
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today=my_tm.tm_mday;
    
    m_fp=fopen(log_full_name,"a");
    if(m_fp==NULL){
        return false;
    }

    return true;
}

void Log::write_log(int level,const char* format,...){
    struct timeval now={0,0};
    gettimeofday(&now,NULL);
    time_t t=now.tv_sec;
    struct tm *sys_tm=localtime(&t);
    struct tm my_tm=*sys_tm;
    char s[16]={0};

    //日志分级
    switch (level)
    {
    case 0:
        strcpy(s,"[debug]:");
        break;
    case 1:
        strcpy(s,"[info]:");
        break;
    case 2:
        strcpy(s,"[warn]:");
        break;
    case 3:
        strcpy(s,"[erro]:");
    
    default:
        strcpy(s,"[info]:");
        break;
    }

    pthread_mutex_lock(m_mutex);

    //更新现有行数
    ++m_count;

    //日志不是今天或写入的日志行数是最大行的倍数
    if(m_today!=my_tm.tm_mday||m_count%m_split_lines==0){
        char new_log[256]={0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16]={0};

        //格式化日志中的时间部分
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        //如果是时间不是今天,则创建今天的日志，更新m_today和m_count
        if(m_today!=my_tm.tm_mday){
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count=0;
        }
        else{
            //超过了最大行，在之前的日志名基础上加后缀, m_count/m_split_lines
             snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp=fopen(new_log,"a");
    }

    pthread_mutex_unlock(m_mutex);

    va_list valst;
    //将传入的format参数赋值给valst,便于格式化输出
    va_start(valst,format);

    string log_str;
    pthread_mutex_lock(m_mutex);

    //写入内容格式：时间 + 内容
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                    my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    //内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)
    int m=vsnprintf(m_buf+n,m_log_buf_size-1,format,valst);
    m_buf[n+m]='\n';
    m_buf[n+m+1]='\0';

    log_str=m_buf;
    pthread_mutex_unlock(m_mutex);

    //若m_is_async为true表示不同步，默认为同步
    //若异步,则将日志信息加入阻塞队列,同步则加锁向文件中写
    if(m_is_async&&!m_log_queue->full()){
        m_log_queue->push(log_str);
    }
    else{
        pthread_mutex_lock(m_mutex);
        fputs(log_str.c_str(),m_fp);
        pthread_mutex_unlock(m_mutex);
    }

    va_end(valst);
}

void Log::flush(){
    pthread_mutex_lock(m_mutex);
    fflush(m_fp);
    pthread_mutex_unlock(m_mutex);
}