#ifndef LST_TIMER
#define LST_TIMER

#include <time.h>
#include "../log/log.h"

class util_timer;
//连接资源
struct client_data
{
    //客户端socket地址
    sockaddr_in address;
    //socket文件描述符
    int sockfd;
    //定时器
    util_timer *timer;
};

class util_timer{
public:
    util_timer():prev(NULL),next(NULL){}

public:
    //超时时间
    time_t expire;
    //回调函数
    void (*cb_func)(client_data*);
    //连接资源
    client_data *uesr_data;
    util_timer *prev;
    util_timer *next;
};

class sort_timer_lst{
public:
    sort_timer_lst():head(NULL),tail(NULL){}
    ~sort_timer_lst(){
        util_timer *tmp=head;
        while(tmp){
            head=tmp->next;
            delete tmp;
            tmp=head;
        }
    }

    void add_timer(ulil_timer *timer){
        if(!timer){
            return;
        }
        if(!head){
            head=tail=timer;
            return;
        }

        //如果新增的定时器时间小于当前头部节点,直接将当前定时器节点作为头部节点
        if(timer->expire<head->expire){
            timer->next=head;
            head->prev=timer;
            head=timer;
            return;
        }

        //其他情况
        add_timer(timer,head);
    }

    //调整定时器,任务发生变化时,调整定时器在链表中的位置
    void adjust_timer(util_timer *timer){
        if(!timer) return;

        util_timer *tmp=timer->next;
        //被调整的定时器在链表尾部或定时器超时值仍然小于下一个定时器超时值,不调整
        if(!tmp||(timer->expire<tmp->expire)) return;


        //被调整的定时器是链表的头部结点,将定时器取出,重新插入
        if(timer==head){
            head=head->next;
            head->prev=NULL;
            timer-next=NULL;
            add_timer(timer,head);
        }
        //被调整的定时器在链表的内部,将定时器取出,重新插入
        else{
            timer->prev->next=timer->next;
            timer->next->prev=timer->prev;
            add_timer(timer,timer->next);
        }
    }

    //删除定时器
    void del_timer(ultil_timer *timer){
        if(!timer) return;

        //链表中只有一个定时器,需要删除该定时器
        if((timer==head)&&(timer==tail)){
            delete timer;
            head=NULL;
            tail=NULL;
            return;
        }

        //被删除的定时器为头结点
        if(timer==head){
            head=head->next;
            head->prev=NULL;
            delete timer;
            return;
        }

        //被删除的定时器为尾结点
        if(timer==tail){
            tail=tail->prev;
            tail->next=NULL;
            delete timer;
        }

        //在链表内部
        timer->prev->next=timer->next;
        timer->next->prev=timer->prev;
        delete timer;
    }

    //定时处理函数
    void tick(){
        if(!head) return;

        //获取当前时间
        time_t cur=time();
        util_timer* tmp=head;

        while(tmp){
            //链表是升序的,如果tmp的超时时间大于cur,后面也不用再查了
            if(cur<tmp->expire) break;

            //定时器到期,调用回调函数
            tmp->cb_func(tmp->user_data);

            //将到期定时器移除并重置头结点
            head=tmp-next;
            if(head){
                head->prev=NULL;
            }
            delete tmp;
            tmp=head;
        }
    }

private:
    void add_timer(util_timer* timer,util_timer *lst_head){
        util_timer* prev=lst_head;
        util_timer* tmp=prev->next;

        //遍历当前结点后的结点直到找到timer对应的位置后插入
        while(tmp){
            if(timer->expire<tmp->expire){
                prev->next=timer;
                timer->next=tmp;
                tmp->prev=timer;
                timer->prev=prev;
                break;
            }
            prev=tmp;
            tmp=tmp->next;
        }

        //遍历完后发现目标定时器需要放在尾结点处
        if(!tmp){
            prev->next=timer;
            timer->prev=prev;
            timer->mext=NULL;
            tail=timer;
        }
    }

private:
    util_timer* head;
    util_timer* tail;
}

#endif