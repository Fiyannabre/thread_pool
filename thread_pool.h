#ifndef _THREAD_POOL_H
#define _THREAD_POOL_H

#ifdef __cplusplus
extern "C"
{
#endif

//线程池
typedef struct _thread_pool thread_pool;

//任务函数
typedef void (*task_func)(void *);

//创建线程池
//1.创建任务队列管理器
//2.创建线程数组
thread_pool *threadpool_create(int thrd_count);

//改变线程池状态，不再分配任务，准备释放线程池
void threadpool_terminate(thread_pool * pool);

//往线程池中push任务
int threadpool_post(thread_pool *pool, task_func func, void *arg);

//释放线程池
//等待所有线程结束
//释放任务管理器
void threadpool_waitdone(thread_pool *pool);

#ifdef __cplusplus
}
#endif

#endif