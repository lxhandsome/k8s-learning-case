# 2. IPC Namespace：进程间通信隔离

> 本节代码：[main.c](./main.c)
> 上一节：[1. UTS Namespace](../1_uts/readme.md)
> 下一节：[3. PID Namespace](../3_pid/readme.md)

## 实验目标

- 使用 `CLONE_NEWIPC` 为容器创建独立的 IPC namespace
- 观察容器内的 System V IPC 对象与宿主机互不可见
- 使用 `ipcmk`、`ipcs` 命令验证消息队列的隔离效果
- 理解 Kubernetes Pod 内共享 IPC namespace 的含义

## 背景与原理

IPC 是 Inter-Process Communication（进程间通信）。内核提供两套 IPC 机制，IPC namespace 都会隔离：

**System V IPC**

| 对象 | 系统调用 | 用途 |
|------|----------|------|
| 消息队列 message queue | `msgget` / `msgsnd` / `msgrcv` / `msgctl` | 进程间按消息收发 |
| 信号量 semaphore | `semget` / `semop` / `semctl` | 进程间同步与互斥 |
| 共享内存 shared memory | `shmget` / `shmat` / `shmdt` / `shmctl` | 大块数据共享，速度最快 |

**POSIX 消息队列**（Linux 2.6.30 起支持）

- 通过 `mq_open` / `mq_send` / `mq_receive` 使用
- 队列通过 `mqueue` 文件系统暴露，挂载后可在 `/dev/mqueue` 查看

这些对象在内核中以 **key/ID** 标识，保存在按 IPC namespace 划分的表中：

- 新 IPC namespace 拥有一张全新的空表
- namespace 内创建的对象，外部 namespace 看不到也访问不到
- namespace 销毁时，其中的 IPC 对象随之回收

IPC namespace **不隔离**以下内容：

- 匿名管道 `pipe`：通过文件描述符继承，与 namespace 无关
- Unix domain socket：归属 network namespace
- 信号 `signal`：与 PID namespace 相关
- `/dev/shm`、`shm_open()` 的 POSIX 共享内存：本质是 tmpfs 文件，归属 mount namespace
- futex、文件映射 `mmap`：不在隔离范围

## 代码解读

本节在 UTS 实验的基础上叠加 `CLONE_NEWIPC`：

```c
int container_pid = clone(container_main, container_stack + STACK_SIZE,
                          CLONE_NEWUTS | CLONE_NEWIPC | SIGCHLD, NULL);
```

| 参数 | 作用 |
|------|------|
| `CLONE_NEWUTS` | 隔离 hostname（保留上一节能力） |
| `CLONE_NEWIPC` | 隔离 System V IPC 与 POSIX 消息队列 |
| `SIGCHLD` | 子进程退出时通知父进程，配合 `waitpid()` 回收 |

> 必须同时保留 `CLONE_NEWUTS`：代码中调用了 `sethostname()`，如果没有创建新的 UTS namespace，以 root 运行会把宿主机的 hostname 改掉。

日志辅助函数：

- `print_namespace()`：分别读取 `/proc/self/ns/uts` 和 `/proc/self/ns/ipc`，打印 namespace 编号
- `print_hostname()`：打印当前 hostname

通过对比宿主机与容器打印的 `ipc:[...]` 编号，可以直接确认两个进程处于不同的 IPC namespace。

## 编译与运行

```bash
cd linux/namespace/2_ipc

gcc -Wall -g main.c -o main
sudo ./main
```

> 创建新的 IPC namespace 需要 `CAP_SYS_ADMIN` 权限，必须使用 `sudo`。

## 预期输出

```text
[宿主机] 父进程 PID = 4210
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] IPC 命名空间: ipc:[4026531839]
[宿主机] hostname = "ubuntu"

[宿主机] 开始创建容器（CLONE_NEWUTS | CLONE_NEWIPC）...
[宿主机] 容器进程在宿主机上的 PID = 4211

[容器] 子进程已进入新的 UTS + IPC 命名空间，PID = 4211
[容器] UTS 命名空间: uts:[4026532445]
[容器] IPC 命名空间: ipc:[4026532446]
[容器] 修改前 hostname = "ubuntu"
[容器] 修改后 hostname = "container"
[容器] 提示：在 shell 中执行 ipcmk -Q 创建消息队列，再用 ipcs -q 查看
[容器] 即将启动 /bin/bash，请在 shell 中验证 IPC 隔离效果
```

## 验证 IPC 隔离

**第一步：在宿主机上准备一个 IPC 对象**

```bash
ipcmk -Q        # 创建一条 System V 消息队列
ipcs -q         # 查看宿主机上的消息队列，记录 key 和 msqid
```

**第二步：运行容器并在容器内查看**

```bash
sudo ./main
# 进入容器内 bash 后：
ipcs -q         # 空表：看不到宿主机上刚创建的消息队列
```

**第三步：在容器内创建 IPC 对象**

```bash
ipcmk -Q        # 在容器的 IPC namespace 中创建消息队列
ipcs -q         # 只能看到容器内创建的这条队列
```

**第四步：回到宿主机对比**

```bash
# 另开一个宿主机终端
ipcs -q         # 仍然只看到宿主机的队列，看不到容器内创建的队列
```

**第五步：退出容器**

```bash
exit            # 容器 PID 1 退出，namespace 销毁，其中的队列一并消失
```

> POSIX 消息队列需要先挂载 mqueue 文件系统才能在 `/dev/mqueue` 中查看：
> `mount -t mqueue none /dev/mqueue`。本实验使用 System V 消息队列即可验证。

## 与 Kubernetes 的对应关系

| 场景 | 行为 |
|------|------|
| 同一 Pod 内多个容器 | 共享同一个 IPC namespace，可通过 System V 共享内存、消息队列通信 |
| 不同 Pod 之间 | IPC namespace 相互隔离，key 相同也互不干扰 |
| `spec.hostIPC: true` | 容器共享节点的 IPC namespace，能看到节点上所有 SysV 对象（慎用） |
| Docker `--ipc` | `private`（默认）/ `shareable` / `container:<id>` / `host` |
| `/dev/shm` | 仍按容器独立挂载，不受 IPC namespace 共享影响 |

## 常见问题

| 现象 | 原因 |
|------|------|
| `clone: Operation not permitted` | 未使用 `sudo`，缺少 `CAP_SYS_ADMIN` |
| 容器内 `ipcs` 能看到宿主机对象 | clone 时缺少 `CLONE_NEWIPC` |
| 宿主机 hostname 被改掉 | clone 时缺少 `CLONE_NEWUTS` |
| 容器内看不到 `/dev/shm` 中的内容 | `/dev/shm` 属于 mount namespace，不属于 IPC namespace |
| POSIX 队列在 `/dev/mqueue` 中看不到 | 未挂载 mqueue 文件系统 |

## 扩展实验：使用 unshare

```bash
# 创建新的 IPC namespace 并进入 bash
sudo unshare --ipc bash

ipcs -q         # 看不到宿主机的 IPC 对象
ipcmk -Q        # 在容器内创建消息队列
ipcs -q         # 只能看到容器内的队列
exit

ipcs -q         # 回到宿主机，仍只看到宿主机的队列
```

## 下一节

[3. PID Namespace](../3_pid/readme.md)：隔离进程号，容器内第一个进程的 PID 为 1。
