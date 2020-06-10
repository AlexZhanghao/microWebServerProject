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

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536           //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5             //最小超时单位

#define SYNSQL //同步数据库校验

#define SYNLOG //同步写日志

//#define ET   //边缘触发非阻塞
#define LT   //水平触发阻塞

extern int addfd(int epollfd,int fd,bool one_shot);
extern int remove(int epollfd,int fd);
extern int setnonblocking(int fd);

//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;