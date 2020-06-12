#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

static pthread_mutex_t *m_mutex=new pthread_mutex_t;

class Log{
public:
    //懒汉模式
    static Log* get_instance(){
        static Log instance;
        return &instance;
    }

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志条队列
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    //异步写日志公有方法
    static void *flush_log_thread(void *args){
        Log::get_instance()->async_write_log();
    }

    //将输出内容按照标准格式整理
    void write_log(int level,const char *format,...);

    //强制刷新缓冲区
    void flush();

private:
    Log();
    virtual ~Log();

    //异步写日志
    void *async_write_log(){
        string single_log;

        //从阻塞队列中取出一条日志内容，写入文件
        while(m_log_queue->pop(single_log)){
            pthread_mutex_lock(m_mutex);
            fputs(single_log.c_str(),m_fp);
            pthread_mutex_unlock(m_mutex);
        }
    }

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //因为按天分类,记录当前时间是那一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;        //要输出的内容
    block_queue<string> *m_log_queue; //阻塞队列
    bool m_is_async;                  //是否同步标志位
};

//这四个宏定义在其他文件中使用，主要用于不同类型的日志输出。
//包括DEBUG，INFO，WARN和ERROR四种级别的日志。
#define LOG_DEBUG(format, ...) Log::get_instance()->write_log(0, format, __VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, __VA_ARGS__)
#define LOG_WARN(format, ...) Log::get_instance()->write_log(2, format, __VA_ARGS__)
#define LOG_ERROR(format, ...) Log::get_instance()->write_log(3, format, __VA_ARGS__)

#endif