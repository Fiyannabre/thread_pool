#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include "thread_pool.h"
#include "spinlock.h"

typedef struct spinlock spinlock_t;

//任务
typedef struct task_s {
    void *next;
    task_func func;
    void *arg;
} task_t;

//任务队列管理器
typedef struct task_queue_s {
    void *head;             //指向第一个任务
    void **tail;            //指向最后一个任务结点的next        
    int block;              //是否为阻塞状态
    spinlock_t lock;        //自旋锁
    pthread_mutex_t mutex;  //条件锁
    pthread_cond_t cond;    //互斥锁
} task_queue;

struct _thread_pool {
    task_queue *task_queue; //任务队列管理器
    atomic_int quit;        //
    int thrd_count;         //线程池线程数量
    pthread_t *threads;
};


void thrdpool_terminate(thread_pool* pool);
static void __taskqueue_destroy(task_queue *queue);
static void __threads_terminate(thread_pool * pool);

//创建任务队列管理器对象
//回滚式编程
//创建锁，如果创建失败就释放之前创建的对象
static task_queue *
__taskqueue_create() {
    int ret;
    task_queue *queue = (task_queue *)malloc(sizeof(task_queue));
    if (queue) {
        ret = pthread_mutex_init(&queue->mutex, NULL);
        if (ret == 0) {
            ret = pthread_cond_init(&queue->cond, NULL);
            if (ret == 0) {
                spinlock_init(&queue->lock);
                queue->head = NULL;
                queue->tail = &queue->head;
                queue->block = 1;
                return queue;
            }
            pthread_mutex_destroy(&queue->mutex);
        }
        free(queue);
    }
    return NULL;
}

//
static void
__nonblock(task_queue *queue) {
    pthread_mutex_lock(&queue->mutex);
    queue->block = 0;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_broadcast(&queue->cond);
}

//添加任务到任务队列尾部
static inline void 
__add_task(task_queue *queue, void *task) {
    // 不限定任务类型，只要该任务的结构起始内存是一个用于链接下一个节点的指针
    void **link = (void**)task;
    //相当于task->next = NULL，但是不需要写出变量名，方便扩展. 需要task第一个变量为next（前8个字节）
    *link = NULL;

    spinlock_lock(&queue->lock);
    //相当于queue->tail->next = task
    *queue->tail = link;
    //相当于queue->tail = task
    queue->tail = link;
    spinlock_unlock(&queue->lock);

    //有新任务了，唤醒阻塞在条件变量上的线程
    pthread_cond_signal(&queue->cond);
}

//任务队列pop
//防御式编程
static inline void * 
__pop_task(task_queue *queue) {
    spinlock_lock(&queue->lock);
    if (queue->head == NULL) {
        spinlock_unlock(&queue->lock);
        return NULL;
    }
    task_t *task;
    task = queue->head;
    
    //相当于queue->head = task->next
    void **link = (void**)task;
    queue->head = *link;
    
    //如果队列为空了重新设置尾指针
    if (queue->head == NULL) {
        queue->tail = &queue->head;
    }
    spinlock_unlock(&queue->lock);
    return task;
}

//阻塞在cond上直到有新任务到达
static inline void * 
__get_task(task_queue *queue) {
    task_t *task;
    // 虚假唤醒
    // 如果任务队列为空就交还锁返回
    while ((task = __pop_task(queue)) == NULL) {
        pthread_mutex_lock(&queue->mutex);
        //如果为非阻塞就不再分配任务
        if (queue->block == 0) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        //归还锁queue->mutex并阻塞在queue->cond上
        //当被唤醒时拥有queue->mutex
        pthread_cond_wait(&queue->cond, &queue->mutex);
        pthread_mutex_unlock(&queue->mutex);
    }
    return task;
}

//线程池的单个线程
//循环获取任务并执行
static void *
__thrdpool_worker(void *pool_) {
    thread_pool *pool = (thread_pool*) pool_;   //属于的线程池
    
    task_t *task;   //任务
    void *args;     //参数

    while (atomic_load(&pool->quit) == 0) {
        task = (task_t*)__get_task(pool->task_queue);
        if (!task) break;
        task_func func = task->func;
        args = task->arg;

        free(task);
        func(args);
    }
    
    return NULL;
}

//线程池初始化指定数量的线程
static int 
__threads_create(thread_pool *pool, size_t thrd_count) {
    pthread_attr_t attr;
	int ret;

    ret = pthread_attr_init(&attr);

    if (ret == 0) {
        //创建线程数组
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thrd_count);
        if (pool->threads) {
            int i = 0;
            for (; i < thrd_count; i++) {
                //创建线程
                if (pthread_create(&pool->threads[i], &attr, __thrdpool_worker, pool) != 0) {
                    break;
                }
            }
            pool->thrd_count = i;
            pthread_attr_destroy(&attr);
            if (i == thrd_count)
                return 0;
            //如果没有创建指定数量的线程就释放之前创建的对象
            __threads_terminate(pool);
            free(pool->threads);
        }
        ret = -1;
    }
    return ret; 
}

//创建线程池
//创建任务队列管理器
//创建线程数组
thread_pool *
thrdpool_create(int thrd_count) {
    thread_pool *pool;

    pool = (thread_pool*)malloc(sizeof(*pool));
    if (pool) {
        task_queue *queue = __taskqueue_create();
        if (queue) {
            pool->task_queue = queue;
            atomic_init(&pool->quit, 0);
            if (__threads_create(pool, thrd_count) == 0)
                return pool;
            __taskqueue_destroy(queue);
        }
        free(pool);
    }
    return NULL;
}

//线程池添加任务
int
thrdpool_post(thread_pool *pool, task_func func, void *arg) {
    if (atomic_load(&pool->quit) == 1) 
        return -1;
    task_t *task = (task_t*) malloc(sizeof(task_t));
    if (!task) return -1;
    task->func = func;
    task->arg = arg;
    __add_task(pool->task_queue, task);
    return 0;
}

//删除任务队列管理器，并释放任务队列
static void
__taskqueue_destroy(task_queue *queue) {
    task_t *task;
    while ((task = __pop_task(queue))) {
        free(task);
    }
    spinlock_destroy(&queue->lock);
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}

//改变线程池状态 准备释放线程池
void
thrdpool_terminate(thread_pool* pool) {
    atomic_store(&pool->quit, 1);
    __nonblock(pool->task_queue);
}

//当申请的线程数量没达到要求时，释放所有线程 设置线程池状态为退出 设置任务队列非阻塞 等待所有线程返回
static void 
__threads_terminate(thread_pool * pool) {
    atomic_store(&pool->quit, 1);
    __nonblock(pool->task_queue);
    int i;
    for (i=0; i<pool->thrd_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
}

void
thrdpool_waitdone(thread_pool *pool) {
    int i;
    for (i=0; i<pool->thrd_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    __taskqueue_destroy(pool->task_queue);
    free(pool->threads);
    free(pool);
}