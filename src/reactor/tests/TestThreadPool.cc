#include "ThreadPool.h"
#include <stdlib.h>
#include <iostream>
#include <memory>
#include <functional>

using std::cout;
using std::endl;
using std::unique_ptr;
using std::bind;

class MyTask
{
public:
    void process()
    {
        //需要做的任务，也就是要处理的业务逻辑
        ::srand(::clock());
        int number = ::rand() % 100;
        cout << ">>BO_Threadpool.MyTask number = " << number << endl;
    }
};

//Q1：线程池中的任务执行不完，程序就退出来了？
//原因：子线程在创建完毕之后，会执行线程入口函数，只要线程池的标志位
//_isExit，那么子线程就一直在doTask函数中进行循环，但是当主线程在
//执行添加任务addTask的时候，如果将任务添加完毕之后，主线程会继续走
//后面的stop函数，那么就会出现子线程在doTask中，主线程在stop中，
//如果主线程运行的要快一些，可能已经将标志位_isExit设置为true，那么
//子线程在执行doTask时候发现循环标志被改了，就跳出循环，所以有可能就
//出现了任务没有执行完毕，程序就跳出来了
//解决问题：可以在stop函数添加_taskQue.empty()操作，也就是只要任务执行
//不完，主线程就不能将标志位_isExit设置为true
//
//
//Q2:任务执行完毕，但是线程池无法退出？
//原因：子线程在执行doTask函数，如果在执行getTask函数获取最后的任务，让
//任务队列为空,那么这个时候主线程会继续向下执行，也就是指向到stop中将
//标志位_isExit设置为true，如果主线程执行的速度要快一些，那么子线程执行的
//速度要慢一些，也就是子线程还在执行process的时候，主线程已经将标志位
//设置为为了_isExit为true，那么程序就可以正常退出；如果子线程在执行完
//getTask获取到最后任务之后，可以很快的执行process，那么就会继续走while
//循环，但是如果此时标志位没有被修改，也就是主线程执行的比较慢，那么
//子线程会继续走getTask，但是任务队列为空，所以子线程就会在_notEmpty
//条件变量上睡眠，此时程序就卡主了
//解决方案：将所有等待在_notEmpty条件变量上的线程全部唤醒,也就是执行
//_notEmpty.notify_all();但是_notEmpty条件变量上睡眠的线程在while
//循环中，此时还是无法退出，所以添加了标志位_flag(该标志位是存在
//TaskQueue类中的，目的就是将唤醒的线程可以从while跳出来，不至于
//继续睡眠)
void test()
{
    unique_ptr<MyTask> ptask(new MyTask());//创建任务

    ThreadPool pool(4, 10);
    //启动线程池
    pool.start();

    int cnt = 20;
    while(cnt--)
    {
        //添加任务
        pool.addTask(bind(&MyTask::process, ptask.get()));
        cout << "cnt = " << cnt << endl;
    }

    //停止线程池
    pool.stop();
}

int main()
{
    test();
    return 0;
}
