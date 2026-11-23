# 3. PID Namespace：进程号隔离

> 本节代码：[main.c](./main.c)
> 上一节：[2. IPC Namespace](../2_ipc/readme.md)
> 下一节：Mount namespace（待完成）

## 实验目标

- 用 `CLONE_NEWPID` 为容器创建独立的 PID namespace
- 观察容器内第一个进程的 PID 为 1，与宿主机 PID 完全独立
- 理解 `/proc` 与 PID namespace 的关系，解释"为什么 `ps` 还能看到宿主机进程"
- 理解 PID 1 的特殊语义，为学习 K8s 容器优雅退出、僵尸进程打基础

## 原理

PID namespace 隔离的是**进程号的编号空间**，而不是进程本身。每个 PID namespace 内部都有一套独立的 PID 编号：

- 新 namespace 中 `clone()` 创建的第一个进程成为该 namespace 的 **init 进程，PID 为 1**
- 进程只能看到自己所在 namespace 及后代 namespace 中的进程，看不到祖先和兄弟 namespace 中的进程
- 同一个进程在不同 namespace 里可以有不同 PID：
  - 容器内看自己是 `1`
  - 宿主机上看它是一个普通的数字（比如 `12346`）
- 父进程拿到的 `clone()` 返回值，始终是**父 namespace 视角**的 PID

命名空间采用叠加的方式：本节在 UTS、IPC 的基础上增加 `CLONE_NEWPID`，前两节的隔离能力继续保留。

## 代码解读

`main.c` 与 IPC 实验结构相同，关键是 clone 的 flag 增加了 `CLONE_NEWPID`：

```c
int container_pid = clone(container_main, container_stack+STACK_SIZE,
                          CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | SIGCHLD,
                          NULL);
```

| 参数 | 作用 |
|------|------|
| `CLONE_NEWUTS` | 隔离 hostname |
| `CLONE_NEWIPC` | 隔离 System V IPC / POSIX 消息队列 |
| `CLONE_NEWPID` | 隔离进程号，子进程成为新 namespace 的 PID 1 |
| `SIGCHLD` | 子进程退出时通知父进程，配合 `waitpid()` 回收；不加的话父进程无法正常 wait |

`container_main()` 里的 `getpid()` 打印出 1；父进程 `main()` 打印的是宿主机 PID。

日志辅助函数：

- `print_namespace()`：分别读取 `/proc/self/ns/uts`、`/proc/self/ns/ipc`、`/proc/self/ns/pid`，打印 namespace 编号，方便与宿主机对比
- `print_hostname()`：打印当前 hostname
- `execv()` 前调用 `fflush(stdout)`，避免 stdout 被重定向时日志滞留在缓冲区中丢失

## 编译与运行

```bash
cd linux/namespace/3_pid

gcc -Wall -g main.c -o main
sudo ./main
```

**必须用 `sudo`**：创建 PID namespace 需要 `CAP_SYS_ADMIN` 权限。普通用户直接运行，`clone()` 会返回 -1（`Operation not permitted`），代码里已用 `perror` 打印该错误。

也可以直接在 VS Code 里按 `Ctrl+Shift+B` 构建。

## 预期输出

```text
[宿主机] 父进程 PID = 4210
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] IPC 命名空间: ipc:[4026531839]
[宿主机] PID 命名空间: pid:[4026531836]
[宿主机] hostname = "ubuntu"

[宿主机] 开始创建容器（CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID）...
[宿主机] 容器进程在宿主机上的 PID = 4211（容器内看到的是 1）

[容器] 子进程已进入新的 UTS + IPC + PID 命名空间
[容器] 子进程 PID = 1（新 PID 命名空间中的 init 进程）
[容器] UTS 命名空间: uts:[4026532445]
[容器] IPC 命名空间: ipc:[4026532446]
[容器] PID 命名空间: pid:[4026532444]
[容器] 修改前 hostname = "ubuntu"
[容器] 修改后 hostname = "container"
[容器] 提示：在 shell 中执行 echo $$ 应输出 1
[容器] 提示：ps 仍会显示宿主机进程，这是正常的（/proc 尚未隔离）
[容器] 即将启动 /bin/bash，请在 shell 中验证 PID 隔离效果
```

进入容器内 bash 后：

```bash
echo $$          # 输出 1，bash 就是新 namespace 的 PID 1
sleep 100 &      # 容器内新进程从 PID 2 开始分配
echo $!          # 输出 2（或 3、4……）
exit             # PID 1 退出 → namespace 内所有进程被内核杀掉
```

退出后父进程打印：

```text
[宿主机] 容器已退出
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] IPC 命名空间: ipc:[4026531839]
[宿主机] PID 命名空间: pid:[4026531836]
[宿主机] hostname = "ubuntu"
```

## 为什么 ps 还能看到宿主机进程？

在容器内执行 `ps -ef`，仍会看到宿主机上的全部进程，这**不是** PID 隔离失效：

- `ps` 读取的是 `/proc`，而 `/proc` 是**宿主机在 mount namespace 中挂载的 procfs**
- procfs 的进程视图由"挂载它的 PID namespace"决定，与当前进程所在的 PID namespace 无关
- 本实验没有创建新的 mount namespace，容器与宿主机共享 `/proc`，所以看到的是宿主机进程列表（显示宿主机视角的 PID）
- 同理，`cat /proc/1/comm` 看到的仍是宿主机 init（如 systemd），而不是容器里的 bash

验证 PID 隔离最可靠的方法：

```bash
echo $$                       # bash 的 getpid()，输出 1
readlink /proc/self/ns/pid    # 对比容器内外 inode，不同即处于不同 namespace
```

要真正隔离 `/proc` 视图，需要 `CLONE_NEWNS` 新建 mount namespace 并重新挂载 proc，这是下一节的内容。

## PID 1 的特殊之处

PID 1 不是普通进程，它在 namespace 内承担 init 职责：

- **信号**：namespace 内部成员发给 PID 1 的信号，若 PID 1 没有注册 handler 会被内核忽略；内部成员也无法用 `SIGKILL`/`SIGSTOP` 杀掉它。来自祖先 namespace（宿主机）的 `SIGKILL` 仍然有效
- **收尸**：namespace 内的孤儿进程会挂到 PID 1 名下，PID 1 需要负责回收僵尸进程
- **生命周期**：PID 1 一旦退出，内核会 `SIGKILL` 该 namespace 内所有进程，namespace 随之销毁

这也是容器实践中常见问题的来源：

- `docker stop` / K8s 删除 Pod 时向容器 PID 1 发送 `SIGTERM`；主进程不处理信号，就只能等宽限期结束后被 `SIGKILL`
- 应用本身不回收子进程，会产生僵尸进程
- 通常用 `tini`、`dumb-init` 作为 PID 1 转发信号、回收僵尸进程

## 与 Kubernetes 的对应关系

| 场景 | 行为 |
|------|------|
| 容器默认 | 每个容器独立 PID namespace，容器主进程为 PID 1 |
| 同一 Pod 内多容器 | 默认各自独立 PID namespace，互相看不到进程 |
| `spec.shareProcessNamespace: true` | Pod 内容器共享 PID namespace，`kubectl exec` 可看到彼此进程 |
| `spec.hostPID: true` | 共享节点 PID namespace，容器内 `ps` 能看到宿主机全部进程（慎用） |
| Docker `--pid=host` | 同上，共享宿主机 PID namespace |
| `kubectl exec` | 进入目标容器的 namespace，`ps` 只看到该容器的进程（镜像需自带 `ps`） |

## 常见问题

| 现象 | 原因 |
|------|------|
| `clone: Operation not permitted` | 未用 `sudo`，缺少 `CAP_SYS_ADMIN` |
| 容器内 `ps` 看到宿主机进程 | 正常，`/proc` 仍来自宿主机 mount namespace（见上文） |
| 容器内 `kill <宿主机 PID>` 失败 | 隔离生效，看不到也杀不了父 namespace 的进程 |
| `exit` 后容器内后台进程全部消失 | PID 1 退出，namespace 被销毁 |
| `waitpid` 一直阻塞 | 容器内 bash 尚未退出；在容器内执行 `exit` 即可 |

## 扩展实验：用 unshare 体验

不写代码，用 util-linux 的 `unshare` 命令：

```bash
# 创建 PID namespace，fork 出的 bash 成为 PID 1
sudo unshare --pid --fork bash
echo $$          # 1
ps -ef           # 仍看到宿主机进程（/proc 未重新挂载）

# 同时重新挂载 /proc，得到正确的 ps 视图
sudo unshare --pid --fork --mount-proc bash
echo $$          # 1
ps -ef           # 只能看到 bash、ps 等少数进程
```

`--fork` 必须加：`unshare` 自身不会进入新的 PID namespace，只有它 fork 出的子进程才成为新 namespace 的 PID 1。

## 下一节

Mount namespace（`CLONE_NEWNS`）：隔离文件系统挂载点，并重新挂载 `/proc`，让 `ps` 也看到隔离后的进程视图。
