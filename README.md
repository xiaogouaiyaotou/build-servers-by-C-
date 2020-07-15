记录一下学习构建web服务器从0-1的过程
## version 1.0
由于什么都不懂，掌握比较困难，在花了大量时间研究别人的代码之后总结出构建web服务器的流程图</br>
此服务器主要运用以下技术和结构
* 使用了epoll边缘触发+EPOLLONESHOT+非阻塞IO
* 使用了固定线程数的线程池
* 实现了由条件变量触发通知新任务到来的任务队列
* 利用小根堆定时器实现了提出超时请求的功能，定时器利用STL优先队列进行管理
* 解析HTTP的GET/POST请求以及支持长短连接
其大概工作流程如下
![image](https://github.com/xiaogouaiyaotou/build-servers-by-Cpp/blob/master/picture/server.jpeg)
