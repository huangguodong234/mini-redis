# mini-redis

用纯 C 语言手写的、兼容 Redis 协议的、内存级键值存储引擎。

## 🎯 项目定位

证明自己“能写底层”的最强证据——从 Socket 网络编程、RESP 协议解析，到哈希表存储引擎，全部手写，不依赖任何第三方库。

## 🚀 快速开始

### 编译
```bash
make clean && make

运行服务器
./server

用 redis-cli 连接测试
redis-cli -h 127.0.0.1 -p 6379
127.0.0.1:6379> SET name zhangsan
OK
127.0.0.1:6379> GET name
"zhangsan"
127.0.0.1:6379> DEL name
(integer) 1

### 📊 性能测试
测试环境
CPU：VMware 虚拟机

内存：2GB

OS：RHEL 8.10

编译器：gcc 8.x

测试工具：redis-benchmark、自研 bench_client

哈希表 vs 动态数组（单连接，原生 C 客户端）
数据量	             动态数组（O(n)）	    哈希表（O(1)）	   提升倍数
100 条 SET	         1.06 秒	            < 0.01 秒	      > 100x
1,000 条 SET	     9 秒	                0.06 秒	           150x
10,000 条 SET	     崩溃（double free）	0.63 秒	            ∞
100,000 条 SET	     推算 ~25 小时	        6.3 秒	           ~14,000x
1,000,000 条 SET	 不可完成	            63 秒	            ∞

📊 动态数组 vs 哈希表：redis-benchmark 压测（10 并发）
请求量	操作	动态数组总耗时	动态数组 QPS	哈希表总耗时	哈希表 QPS
100	    SET	      0.05s	        1886	         0.05s	      2083
100	    GET	      0.05s	        2173	         0.05s	      2000
1000	SET	      0.47s	        2150	         0.45s	      2217
1000	GET	      0.45s	        2212	         0.43s	      2314
10000	SET	      4.39s	        2275	         4.14s	      2417
10000	GET	      4.64s	        2152	         4.02s	      2485
100000	SET	      52.09s	    1919	         42.38s	      2359
100000	GET	      50.94s	    1962	         41.94s	      2384
1000000	SET	      531.86s	    1880	         407.45s	  2454
1000000	GET	      474.08s	    2109	         409.88s	  2439

## 🏗️ 项目架构
mini-redis/
├── include/
│   ├── protocol.h      # 协议解析接口
│   ├── storage.h        # 存储层接口
│   └── hashtable.h      # 哈希表接口
├── src/
│   ├── server.c         # 服务器主程序（Socket + 事件循环）
│   ├── protocol.c       # RESP 协议解析器
│   ├── storage.c        # 存储层（封装哈希表）
│   ├── hashtable.c      # 哈希表实现（djb2 + 链地址法）
│   ├── test_hash.c      # 哈希分布测试
│   ├── test_client.c    # 功能测试客户端
│   └── bench_client.c   # 原生 C 压测客户端
├── Makefile
└── README.md

## 架构分层
┌─────────────────────────┐
│   网络层 (server.c)      │  ← Socket 通信、事件循环
├─────────────────────────┤
│   协议层 (protocol.c)   │  ← RESP 协议解析
├─────────────────────────┤
│   存储层 (storage.c)    │  ← 封装哈希表操作
├─────────────────────────┤
│   数据结构层 (hashtable.c) │  ← djb2 哈希 + 链地址法
└─────────────────────────┘

## 🧠 核心技术点
已实现
TCP 服务器（socket/bind/listen/accept）

RESP 协议解析（SET/GET/DEL 命令）

动态数组存储引擎（阶段 1，已废弃）

djb2 哈希函数

链地址法哈希表

哈希表替代动态数组，性能提升 > 10,000 倍

valgrind 零内存泄漏

SIGINT 信号处理，优雅退出

待实现
哈希表自动扩容（负载因子触发）

有序集合（ZADD/ZRANGE/ZREM），基于跳表

AOF 持久化

多客户端并发（epoll）

##📝 开发日志
阶段 1：启动与存储（5.28 - 5.31）
从零搭建 TCP echo 服务器

实现 RESP 协议解析

动态数组存储引擎上线

阶段 2：提速与重构（6.1 - 6.7）
实现 djb2 哈希表

用哈希表替换动态数组

性能测试：哈希表比动态数组快 10,000 倍

阶段 3：进阶与深化（6.8 - 暑假）
跳表实现有序集合

代码模块化与文档完善

🛠️ 开发工具
编译器：gcc

构建工具：make

内存检查：valgrind

压测工具：redis-benchmark

版本控制：Git + GitHub

📄 许可证
MIT License
EOF
