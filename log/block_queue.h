#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include<iostream>
#include<stdlib.h>
#include<pthread.h>
#include<sys/time.h>

using namespace std;

template<typename T>
class block_queue{
public:
    block_queue(int max_size=1000){
        if(max_size<=0){
            exit(-1);
        }

        //构造函数创建循环数组
        m_max_size=max_size;
        m_array=new T[max_size];
        m_size=0;
        m_front=-1;
        m_back=-1;

        //创建互斥锁和条件变量
        m_mutex=new pthread_mutex_t;
        m_cond=new pthread_cond_t;
        pthread_mutex_init(m_mutex,NULL);
        pthread_cond_init(m_cond,NULL);
    }

    ~block_queue(){
        pthread_mutex_lock(m_mutex);
        if(m_array!=NULL){
            delete m_array;
        }
        pthread_mutex_unlock(m_mutex);

        pthread_mutex_destroy(m_mutex);
        pthread_cond_destroy(m_cond);

        delete m_mutex;
        delete m_cond;
    }

    //清空数组,这里只是将计数器更改了
    void clear(){
        pthread_mutex_lock(m_mutex);
        m_size = 0;
        m_front = -1;
        m_back = -1;
        pthread_mutex_unlock(m_mutex);
    }

    bool full() const{
        pthread_mutex_lock(m_mutex);
        if(m_size>=m_max_size){
            pthread_mutex_unlock(m_mutex);
            return true;
        }
        pthread_mutex_unlock(m_mutex);
        return false;
    }

    bool empty() const{
        pthread_mutex_lock(m_mutex);
        if (m_size==0)
        {
            pthread_mutex_unlock(m_mutex);
            return true;
        }
        pthread_mutex_unlock(m_mutex);
        return false;
    }

    bool front(T& value) const{
        pthread_mutex_lock(m_mutex);
        if (0 == m_size)
        {
            pthread_mutex_unlock(m_mutex);
            return false;
        }
        value=m_array[m_front];
        pthread_mutex_unlock(m_mutex);
        return true;
    }

    bool back(T &value) const{
        pthread_mutex_lock(m_mutex);
        if (0 == m_size)
        {
            pthread_mutex_unlock(m_mutex);
            return false;
        }
        value=m_array[m_back];
        pthread_mutex_unlock(m_mutex);
        return true;
    }

    int size() const
    {
        int tmp = 0;
        pthread_mutex_lock(m_mutex);
        tmp = m_size;
        pthread_mutex_unlock(m_mutex);
        return tmp;
    }

    int max_size() const
    {
        int tmp = 0;
        pthread_mutex_lock(m_mutex);
        tmp = m_max_size;
        pthread_mutex_unlock(m_mutex);
        return tmp;
    }
    
    //当有元素push进队列，相当于生产者生产了一个元素,往队列添加元素，需要将所有使用队列的线程先唤醒
    //若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item){
        pthread_mutex_lock(m_mutex);
        if(m_size>=m_max_size){
            pthread_cond_broadcast(m_cond);
            pthread_mutex_unlock(m_mutex);
            return false;
        }

        //将新增数据放在循环数组的对应位置,这里使用了循环数组来模拟队列 
        m_back=(m_back+1)%m_max_size;
        m_array[m_back]=item;
        ++m_size;

        pthread_cond_broadcast(m_cond);
        pthread_mutex_unlock(m_mutex);

        return true;
    }

    //如果当前队列没有元素,则等待条件变量
    bool pop(T& item){
        pthread_mutex_lock(m_mutex);

        //有多个消费者的时候，这里要是用while而不是if
        while(m_size<=0){
            //重新抢到互斥锁时，pthread_cond_wait返回为0
            if(pthread_cond_wait(m_cond,m_mutex)){
                pthread_mutex_unlock(mutex);
                return false;
            }
        }

        //取出队列首的元素
        m_front=(m_front+1)%m_max_size;
        item = m_array[m_front];
        --m_size;
        pthread_mutex_unlock(m_mutex);
        return true;
    }

private:
    pthread_mutex_t *m_mutex;
    pthread_cond_t *m_cond;
    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif