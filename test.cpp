#include<iostream>
#include<functional>
#include<thread>
#include<future>
#include "threadpool.h"
#include<chrono>
using namespace std;

/*
如何让线程池提交任务更加方便
1.pool.submitTask(sum1,10,20);
  pool.submitTask(sum2,10,20,30)
  可变参模板编程
2.我们自己造了Result以及相关类
  package_task（函数对象）  async
  使用future来代替Result节省线程池代码

*/
int sum(int a, int b) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
	return a + b;
}
int main() {
	ThreadPool pool;
    //pool.setMode(PoolMode::MODE_CACHED);
    pool.start(2);
    future<int> res1 = pool.submitTask(sum, 10, 20);
    future<int> res2 = pool.submitTask(sum, 10, 20);
    future<int> res3 = pool.submitTask(sum, 10, 20);
    future<int> res4 = pool.submitTask(sum, 10, 20);
    future<int> res5 = pool.submitTask([](int a, int b)->int {return a + b; }, 20, 20);
    cout << res1.get() << endl;
    cout << res2.get() << endl;
    cout << res3.get() << endl;
    cout << res4.get() << endl;
    cout << res5.get() << endl;

}