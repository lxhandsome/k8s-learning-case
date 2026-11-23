# 1. UTS Namespace：主机名隔离

> 本节代码：[main.c](./main.c)
> 上一节：无
> 下一节：[2. IPC Namespace](../2_ipc/readme.md)

## 实验目标

- 使用 `CLONE_NEWUTS` 为容器创建独立的 UTS namespace
- 观察 `sethostname()` 只修改容器内的 hostname，宿主机不受影响
- 通过 `/proc/self/ns/uts` 对比宿主机与容器的 namespace 编号
- 理解 Kubernetes Pod 的 hostname 来源

## 背景与原理

UTS 是 "UNIX Time-sharing System" 的缩写，每个进程都归属于一个 UTS namespace，其中保存着 `uname(2)` 返回的标识信息：

```c
struct utsname {
    char sysname[];    // 内核名称，如 "Linux"
    char nodename[];   // 主机名 hostname        ← 被隔离
    char release[];    // 内核版本
    char version[];    // 内核编译版本
    char machine[];    // 硬件架构
    char domainname[]; // NIS 域名               ← 被隔离
};
```

其中真正按 namespace 隔离、且允许修改的是 `nodename`（hostname）和 `domainname`。

- `clone(CLONE_NEWUTS)` 时，子进程获得一份 UTS namespace 拷贝
- 子进程中调用 `sethostname()` 只修改这份拷贝
- 宿主机和其他 namespace 中的进程看不到该修改
- namespace 中最后一个进程退出后，该 namespace 被销毁，修改随之消失

## 代码解读

创建容器：

```c
int container_pid = clone(container_main, container_stack + STACK_SIZE,
                          CLONE_NEWUTS | SIGCHLD, NULL);
```

| 参数 | 作用 |
|------|------|
| `CLONE_NEWUTS` | 创建新的 UTS namespace |
| `SIGCHLD` | 子进程退出时通知父进程，配合 `waitpid()` 回收 |

日志辅助函数：

- `print_hostname()`：通过 `gethostname()` 打印当前 hostname
- `print_namespace()`：读取 `/proc/self/ns/uts`，打印当前 namespace 编号（如 `uts:[4026532445]`）。

宿主机与容器打印出的编号不同，就说明 UTS namespace 隔离成功。

容器内修改 hostname：

```c
sethostname("container", 10);
```

`execv()` 会使用 `/bin/bash` 替换容器进程。替换前调用了 `fflush(stdout)`，避免 stdout 被重定向时日志滞留在缓冲区中丢失。

## 编译与运行

```bash
cd linux/namespace/1_uts

gcc -Wall -g main.c -o main
sudo ./main
```

> 创建新的 UTS namespace 需要 `CAP_SYS_ADMIN` 权限，必须使用 `sudo`。普通用户运行会得到 `clone: Operation not permitted`。

## 预期输出

```text
[宿主机] 父进程 PID = 4210
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] hostname = "ubuntu"

[宿主机] 开始创建容器（CLONE_NEWUTS）...
[宿主机] 容器进程在宿主机上的 PID = 4211

[容器] 子进程已进入新的 UTS 命名空间，PID = 4211
[容器] UTS 命名空间: uts:[4026532445]
[容器] 修改前 hostname = "ubuntu"
[容器] 修改后 hostname = "container"
[容器] 即将启动 /bin/bash，请在 shell 中执行 hostname 验证
```

进入容器内 bash 后：

```bash
hostname                      # 输出 container
readlink /proc/self/ns/uts    # 与宿主机的编号不同
exit
```

退出后：

```text
[宿主机] 容器已退出
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] hostname = "ubuntu"
```

## 验证要点

1. 容器内 `hostname` 输出 `container`
2. 宿主机上另开终端执行 `hostname`，仍是原主机名
3. 容器内 `readlink /proc/self/ns/uts` 的编号与宿主机不同
4. 容器退出后宿主机 hostname 不变

> 注意：如果 clone 时**不加** `CLONE_NEWUTS` 却在子进程里调用 `sethostname()`，以 root 运行时会把宿主机的 hostname 真正改掉，属于危险操作。

## 与 Kubernetes 的对应关系

| 场景 | 行为 |
|------|------|
| Pod 默认 hostname | Pod 名 |
| `spec.hostname` | 自定义 Pod 的 hostname |
| `spec.setHostnameAsFQDN` | 将 hostname 设置为 FQDN |
| 同一 Pod 内多个容器 | 共享同一个 UTS namespace，hostname 相同 |
| Docker 容器 | 默认 hostname 为容器 ID 短格式，`-h/--hostname` 可自定义 |

## 常见问题

| 现象 | 原因 |
|------|------|
| `clone: Operation not permitted` | 未使用 `sudo`，缺少 `CAP_SYS_ADMIN` |
| 宿主机 hostname 被改掉 | clone 时缺少 `CLONE_NEWUTS` |
| 容器退出后 hostname 恢复 | namespace 已销毁，属于正常现象 |
| `print_namespace` 显示读取失败 | `/proc` 未挂载或内核未启用 namespace 支持 |

## 扩展实验：使用 unshare

```bash
# 创建新的 UTS namespace 并进入 bash
sudo unshare --uts bash

hostname container    # 修改容器内 hostname
hostname              # 输出 container
exit

hostname              # 宿主机 hostname 未变
```

## 下一节

[2. IPC Namespace](../2_ipc/readme.md)：隔离 System V 消息队列、信号量、共享内存和 POSIX 消息队列。
