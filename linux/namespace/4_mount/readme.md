# 4. Mount Namespace：文件系统挂载隔离

> 本节代码：[main.c](./main.c)
> 上一节：[3. PID Namespace](../3_pid/readme.md)
> 下一节：User Namespace（待完成）

## 实验目标

- 使用 `CLONE_NEWNS` 创建独立的 mount namespace
- 理解挂载传播（mount propagation），以及为什么必须先设为 private
- 在容器内重新挂载 `/proc`，让 `ps` 看到隔离后的进程视图
- 使用 tmpfs 验证容器内文件系统与宿主机隔离
- 理解容器 rootfs、volume 挂载的底层机制

## 背景与原理

Mount namespace 隔离的是**挂载表**（mount table）：每个 namespace 中的进程看到的是不同的目录树挂载结构。

- `clone(CLONE_NEWNS)` 时，子进程获得父进程挂载表的一份拷贝
- 之后子进程执行 `mount()` / `umount()` 只影响自己的 namespace
- 文件系统本身没有被复制，隔离的只是"挂载关系"；同一块磁盘可以被多个 namespace 以不同方式挂载
- 容器的 rootfs（overlayfs）、`/proc`、`/sys`、`/dev/shm` 以及各类 volume 都建立在此机制之上

### Linux 文件系统与挂载基础

理解 mount namespace 之前，先分清几个概念：

```text
块设备 /dev/sdX
  └── 文件系统 ext4（由 mkfs.ext4 创建）
        └── 根目录 /
              └── from_container.txt
```

| 概念 | 说明 |
|------|------|
| 块设备 | 提供原始存储的设备，如 `/dev/sdb1`、LVM 卷、loop 设备。它本身只是扇区集合，未格式化前不能直接存文件 |
| 文件系统 | 块设备上的数据结构（ext4、xfs、btrfs 等），负责目录、文件和空闲空间管理，由 `mkfs` 创建 |
| 挂载点 | 目录树中的一个目录，作为访问文件系统的入口 |
| 挂载 | 把文件系统根目录接到挂载点上的操作，通过 `mount(2)` 或 `mount(8)` 完成 |
| mount namespace | 每个 namespace 一张独立挂载表，决定该 namespace 内路径如何解析 |

几个关键点：

- Linux 的所有文件系统组合成**一棵统一的目录树**，`mount` 是把另一棵树接到某个目录上
- **mount 不是复制**：它只是引用一个已经存在的文件系统实例（superblock）
- 挂载后，挂载点原来的目录内容会被**覆盖**（在当前 namespace 中不可见）；其他 namespace 不受影响
- 同一个文件系统可以被多个挂载点、多个 namespace 同时引用，对数据的修改是共享的
- **卸载不是删除**：`umount` 只是断开引用；数据仍然在后备存储上。tmpfs 是例外，最后一个引用消失后内存数据释放
- 容器不是虚拟机：容器共享宿主机内核，容器内写块设备就是写宿主机的同一个块设备

用"父进程创建 `/a/b`，子进程把设备挂载到 `/a/b`"的场景走一遍：

```bash
mkdir -p /a/b              # 在宿主机根文件系统上创建普通目录
mount /dev/sdX /a/b        # 在子进程的 mount namespace 中挂载 sdX 上的 ext4
echo hi > /a/b/file.txt    # 数据写入 sdX 上的 ext4 文件系统
```

此时：

```text
父进程（宿主机 namespace）：
  /a/b  →  宿主机根文件系统上的原目录（空）

子进程（容器 namespace）：
  /a/b  →  /dev/sdX 上的 ext4 根目录  →  file.txt
```

父进程通过 `/a/b` 看到的是原来的空目录，因为它的挂载表里没有这次挂载；文件实际在 `/dev/sdX` 上。父进程要看到它，需要在宿主机上再挂载一次该设备：

```bash
mount /dev/sdX /mnt/container_disk
ls /mnt/container_disk     # file.txt
```

如果宿主机之前已经把 `/dev/sdX` 挂在 `/data`，那么通过 `/data` 可以看到容器写入的文件（同一个 superblock）；namespace 只改变路径解析，不复制数据。

> 一个直观实验：先在宿主机 `/a/b` 里放一个 `host.txt`，再在子进程里挂载设备。容器内看不到 `host.txt`（被设备内容覆盖），宿主机看不到设备里的 `file.txt`；容器退出、挂载消失后，`/a/b` 又变回原来的目录。这证明挂载是"覆盖"而不是"删除"。

> 一句话：**数据放在哪里由后备存储决定，能不能看到由 mount namespace 的挂载表决定。**

### 挂载传播（mount propagation）

这是 mount namespace 最容易踩坑的地方：

- systemd 默认把 `/` 标记为 **shared**，挂载事件会在 peer group 之间传播
- 新建 mount namespace 时会继承这些传播属性
- 如果不先改为私有，容器内 `mount` 一个 tmpfs 或重新挂载 `/proc`，**可能传播到宿主机**
- 因此容器启动后通常第一件事就是：

  ```c
  mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
  ```

查看宿主机根挂载的传播属性：

```bash
findmnt -o TARGET,PROPAGATION /
```

### `/proc` 与 PID namespace

上一节遗留的问题是：PID namespace 已经隔离了进程号，但容器内 `ps` 仍能看到宿主机进程，因为 `/proc` 是宿主机在旧 mount namespace 中挂载的。

procfs 在**挂载时**与当时的 PID namespace 绑定。在新的 mount namespace 中重新挂载 procfs 后：

- `ps` 只能看到新 PID namespace 内的进程
- `/proc/1` 指向容器自己的 init 进程（本例中的 bash）
- 进程号也变成容器视角的小数字

### 同一路径，不同挂载表：为什么宿主机看不到容器内的文件？

这是 mount namespace 的核心，值得单独强调：

- 父进程在**宿主机 mount namespace** 中创建 `/tmp/k8s_ns_mount_demo`，它指向宿主机文件系统上的一个空目录
- 子进程 `clone(CLONE_NEWNS)` 后，拿到的是父进程挂载表的一份**拷贝**
- 子进程把一个新的 tmpfs 实例挂到这个路径上，这个操作只修改子进程自己的挂载表
- 父进程的挂载表没有任何变化，路径解析仍然指向原来的空目录

因此，同一个路径字符串在两个 namespace 中解析到了不同对象：

```text
容器内：/tmp/k8s_ns_mount_demo  →  容器挂载的 tmpfs 根目录  →  container_file.txt
宿主机：/tmp/k8s_ns_mount_demo  →  宿主机文件系统上的空目录  →  （没有文件）
```

关键结论：

1. **不是"容器结束后删除了文件"**。容器运行期间，宿主机就看不到这个文件；文件从未出现在宿主机的目录里
2. 容器退出后 mount namespace 销毁，tmpfs 挂载消失，内存中的数据被释放，路径重新只指向原来的空目录
3. 可以用 `/proc/self/mountinfo` 对比容器内外的挂载表：

   ```bash
   # 容器内
   grep k8s_ns_mount_demo /proc/self/mountinfo   # 有记录
   # 宿主机
   grep k8s_ns_mount_demo /proc/self/mountinfo   # 没有记录
   ```

> 补充：宿主机 root 仍可以通过 `/proc/<容器进程在宿主机上的 PID>/root/tmp/k8s_ns_mount_demo/` 访问容器内的文件，因为 `/proc/<pid>/root` 会按目标进程的 mount namespace 解析路径。这是特权调试视角，不属于宿主机的普通路径视图。

### 这是 tmpfs 的特性吗？

**隔离来自 mount namespace，不是 tmpfs。**

- tmpfs 的贡献：数据存放在内存、没有磁盘后备、随最后一个挂载引用消失而释放。这解释了"容器退出后数据没了"
- 但"宿主机看不到"在容器**运行期间**就成立，与 tmpfs 无关。换成 ext4 镜像或其他文件系统，只要挂载发生在新的 mount namespace 中，宿主机同样看不到
- 反过来，如果在**同一个** mount namespace 中（不创建新的 namespace）挂载 tmpfs，宿主机立刻就能看到这个挂载和文件；`umount` 之后文件才消失
- 另外，宿主机的 `/tmp` 本身也可能是一个 tmpfs，但那是**另一个 tmpfs 实例**；容器里挂载的是全新的 superblock，二者不共享数据

一句话：**mount namespace 决定"看不看得见"，tmpfs 只决定"数据存在哪里、何时释放"。**

> 挂载传播例外：如果挂载没有设为 private，且挂载点在 shared 传播链上，挂载事件可能传播到宿主机 namespace，宿主就能看到。这也是代码里必须先执行 `MS_REC | MS_PRIVATE` 的原因。

### 如果挂载的是持久化文件系统，容器退出后文件还在吗？

把场景换成块设备上的 ext4（而不是 tmpfs）：

- 父进程创建目录 `/a/b`
- 子进程在新的 mount namespace 中把 `/dev/sdX`（ext4）挂载到 `/a/b`
- 子进程写入 `/a/b/from_container.txt`
- 容器退出

这里要回答两个**不同**的问题：

1. **数据还在不在？** 在。文件已经写到块设备上的 ext4 文件系统里，不会因为 mount namespace 销毁而消失
2. **父进程能不能通过 `/a/b` 看到？** 不能。子进程的挂载只存在于子进程的 mount namespace；namespace 销毁时只是**卸载**，不是删除数据。父进程的 `/a/b` 仍然指向宿主机原来的空目录，从未包含这个文件

要让父进程看到文件，需要在父进程的 namespace 中给这个设备建立挂载：

```bash
mkdir -p /mnt/container_disk
mount /dev/sdX /mnt/container_disk
ls /mnt/container_disk        # from_container.txt 在这里
```

如果这个设备在宿主机上**已经挂载**在 `/data`，那么父进程可以通过 `/data` 看到文件（即使容器还在运行），因为两次挂载引用的是同一个文件系统实例（superblock）。namespace 只决定"路径怎么解析"，不会复制数据。

| 子进程挂载的类型 | 容器退出后数据 | 父进程通过 `/a/b` 可见 | 父进程如何看到 |
|------|------|------|------|
| tmpfs | 随 namespace 销毁而释放 | 否 | 无法恢复 |
| 持久化块设备（宿主机未挂载） | 保留在设备上 | 否 | 父进程挂载该设备后可见 |
| 持久化设备（宿主机已挂在 `/data`） | 保留在设备上 | 否 | 通过 `/data` 可见（同一 superblock） |
| bind mount 的宿主机目录 | 保留在源目录 | 否 | 通过源目录可见 |
| 未 private 的共享传播 | 取决于介质 | 可能可见（挂载被传播） | 用 `MS_PRIVATE` 避免 |

**结论：持久性由后备存储决定，可见性由 mount namespace + 挂载表决定，两者互不替代。**

#### 动手实验：用镜像文件模拟持久化设备

> 下面用镜像文件模拟块设备，避免误操作真实磁盘；如果使用真实设备，请先确认设备未被系统占用。

```bash
# 1. 宿主机准备一个 64MB 镜像文件，并格式化为 ext4
dd if=/dev/zero of=/tmp/persist.img bs=1M count=64
mkfs.ext4 -F /tmp/persist.img
mkdir -p /a/b /mnt/persist

# 2. 在新的 mount namespace 中模拟容器
sudo unshare --mount --propagation private bash

mount -o loop /tmp/persist.img /a/b
echo "hello from container" > /a/b/from_container.txt
exit

# 3. 回到宿主机：通过 /a/b 看不到文件
ls -A /a/b                       # 空

# 4. 把同一个设备挂载到宿主机另一个路径，就能看到
sudo mount -o loop /tmp/persist.img /mnt/persist
ls /mnt/persist                  # from_container.txt
cat /mnt/persist/from_container.txt
sudo umount /mnt/persist

# 5. 清理
rm /tmp/persist.img
```

`mount -o loop` 需要 root，在部分受限容器环境中可能不可用；它只是把镜像文件模拟成块设备，便于实验。

#### 容易混淆的几个点

1. **mount 不是复制**：它把同一个文件系统实例挂进当前 namespace 的目录树；多个 namespace 可以挂载同一个实例，写入彼此可见
2. **namespace 销毁 ≠ 数据删除**：对磁盘文件系统只是卸载；对 tmpfs 才意味着内存数据释放
3. **挂载点目录只是锚点**：挂载后原始目录被"盖住"，但只在当前 namespace 内被盖住；其他 namespace 仍看到原目录
4. **持久性 ≠ 可见性**：数据在设备上，但没有挂载路径就看不到；有挂载路径但介质是 tmpfs，退出后数据也会消失
5. **tmpfs 的特殊性只在存储介质和生命周期**：它是内存文件系统、随最后一个挂载引用释放；隔离能力仍来自 mount namespace

## 代码解读

本节在前一节基础上叠加 `CLONE_NEWNS`：

```c
int container_pid = clone(container_main, container_stack + STACK_SIZE,
                          CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                          NULL);
```

| 参数 | 作用 |
|------|------|
| `CLONE_NEWUTS` | 隔离 hostname |
| `CLONE_NEWIPC` | 隔离 System V IPC / POSIX 消息队列 |
| `CLONE_NEWPID` | 隔离进程号，子进程成为新 namespace 的 PID 1 |
| `CLONE_NEWNS` | 隔离挂载表，容器内的挂载不影响宿主机 |
| `SIGCHLD` | 子进程退出时通知父进程，配合 `waitpid()` 回收 |

### `mount()` 函数参数详解

代码中的三次挂载都调用 `mount(2)` 系统调用，原型如下：

```c
#include <sys/mount.h>

int mount(const char *source,
          const char *target,
          const char *filesystemtype,
          unsigned long mountflags,
          const void *data);
```

| 参数 | 含义 | 本实验中的取值 |
|------|------|----------------|
| `source` | 文件系统来源。含义由文件系统类型决定：块设备填设备路径；`tmpfs` / `proc` 可填 `"tmpfs"` / `"proc"` 或 `"none"`；bind mount 时填源路径 | `"proc"`、`"tmpfs"`、`NULL` |
| `target` | 挂载点路径，必须已经存在；挂载目录文件系统时必须是目录 | `"/proc"`、`MOUNT_POINT`、`"/"` |
| `filesystemtype` | 文件系统类型，例如 `"ext4"`、`"xfs"`、`"tmpfs"`、`"proc"`、`"overlay"`、`"nfs"`；bind mount 时可传 `NULL` 或 `"none"` | `"proc"`、`"tmpfs"`、`NULL` |
| `mountflags` | `MS_*` 标志按位或组合，控制挂载属性、传播模式、bind/remount 等行为 | 见下表 |
| `data` | 文件系统私有选项字符串，格式由具体文件系统决定；不需要时传 `NULL` | `NULL`、`"size=10m"` |

返回值：成功返回 `0`；失败返回 `-1` 并设置 `errno`。调用需要 `CAP_SYS_ADMIN` 权限。

本实验用到的标志：

| 标志 | 含义 |
|------|------|
| `MS_REC` | 递归操作，作用于目标挂载点下的所有子挂载 |
| `MS_PRIVATE` | 私有传播：本 namespace 内的 mount/umount 不传播到其他 namespace |
| `MS_NOSUID` | 忽略该文件系统上可执行文件的 setuid/setgid 位 |
| `MS_NODEV` | 禁止访问该文件系统上的设备文件 |
| `MS_NOEXEC` | 禁止在该文件系统上执行程序 |

常见但本例未使用的标志：

| 标志 | 含义 |
|------|------|
| `MS_RDONLY` | 只读挂载 |
| `MS_BIND` | bind mount，把已有路径挂到另一个位置 |
| `MS_REMOUNT` | 修改已有挂载的标志 |
| `MS_SHARED` / `MS_SLAVE` / `MS_UNBINDABLE` | 其他挂载传播模式 |
| `MS_NOATIME` / `MS_RELATIME` | 控制访问时间更新策略 |

对照代码中的三次调用：

```c
/* ① 不挂载新文件系统，只修改 "/" 及其子挂载的传播属性 */
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);

/* ② 在 /proc 挂载新的 procfs，绑定到当前 PID namespace */
mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

/* ③ 在挂载点挂载一个新的 tmpfs，大小上限 10MB */
mount("tmpfs", MOUNT_POINT, "tmpfs", MS_NOSUID | MS_NODEV, "size=10m");
```

- 第 ① 次调用没有新文件系统，`source`、`filesystemtype`、`data` 都为 `NULL`，内核把 `target` 当作已有挂载点，按 `MS_REC | MS_PRIVATE` 修改其传播属性
- 第 ② 次中 `source` 对 procfs 没有实际意义，填 `"proc"` 只是为了可读性
- 第 ③ 次中 `data` 是 tmpfs 专有选项，`"size=10m"` 限制该实例最大使用 10MB 内存

> 注意区分 `mount(2)` 系统调用与 `mount(8)` 命令：后者是 util-linux 提供的工具，会解析 `/etc/fstab`、设备名、选项字符串等，最终也是调用 `mount(2)`。

容器内的 `setup_mount_namespace()` 依次完成三步：

1. **私有化挂载传播**

   ```c
   mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
   ```

   递归地把当前 namespace 的所有挂载点标记为 private，切断向宿主机传播挂载事件的通道。

2. **重新挂载 `/proc`**

   ```c
   mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
   ```

   新的 procfs 与当前（新的）PID namespace 绑定，`ps` 由此获得正确的进程视图。

3. **挂载 tmpfs 并写入标记文件**

   ```c
   mount("tmpfs", MOUNT_POINT, "tmpfs", MS_NOSUID | MS_NODEV, "size=10m");
   ```

   挂载点为 `/tmp/k8s_ns_mount_demo`，由父进程提前创建；tmpfs 只存在于容器的 mount namespace。

父进程在容器退出后检查标记文件 `container_file.txt` 是否存在，以此验证挂载隔离成功。

日志辅助函数：

- `print_namespace()`：读取 `/proc/self/ns/{uts,ipc,pid,mnt}`，打印 namespace 编号
- `print_hostname()`：打印当前 hostname
- `fflush(stdout)`：在 `clone()` 前后和 `execv()` 前刷新缓冲区，保证重定向时日志顺序正确

## 编译与运行

```bash
cd linux/namespace/4_mount

gcc -Wall -g main.c -o main
sudo ./main
```

**必须用 `sudo`**：创建 mount namespace 以及执行 `mount()` 都需要 `CAP_SYS_ADMIN` 权限。普通用户运行会得到 `clone: Operation not permitted`。

> 安全提示：示例会先执行 `MS_PRIVATE` 再挂载。**不要**在宿主机上直接执行示例中的 `mount` 命令，否则可能影响宿主机的 `/proc` 或留下挂载点。

## 预期输出

```text
[宿主机] 父进程 PID = 5210
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] IPC 命名空间: ipc:[4026531839]
[宿主机] PID 命名空间: pid:[4026531836]
[宿主机] Mount 命名空间: mnt:[4026531841]
[宿主机] hostname = "ubuntu"
[宿主机] 容器挂载点：/tmp/k8s_ns_mount_demo（宿主机上保持为空目录）

[宿主机] 开始创建容器（CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS）...
[宿主机] 容器进程在宿主机上的 PID = 5211（容器内看到的是 1）

[容器] 子进程已进入新的 UTS + IPC + PID + Mount 命名空间
[容器] 子进程 PID = 1（新 PID 命名空间中的 init 进程）
[容器] UTS 命名空间: uts:[4026532445]
[容器] IPC 命名空间: ipc:[4026532446]
[容器] PID 命名空间: pid:[4026532444]
[容器] Mount 命名空间: mnt:[4026532447]
[容器] 修改前 hostname = "ubuntu"
[容器] 修改后 hostname = "container"
[容器] 挂载传播已设为私有（MS_PRIVATE），容器内的挂载不会影响宿主机
[容器] 已重新挂载 /proc，ps 将只显示本 PID namespace 内的进程
[容器] 已在 /tmp/k8s_ns_mount_demo 挂载 tmpfs（大小 10MB）
[容器] 已写入标记文件 /tmp/k8s_ns_mount_demo/container_file.txt
[容器] 提示：执行 ps -ef 只会看到容器内的进程
[容器] 提示：执行 mount | grep k8s_ns_mount_demo 可查看 tmpfs
[容器] 提示：执行 cat /tmp/k8s_ns_mount_demo/container_file.txt 可读取标记文件
[容器] 即将启动 /bin/bash，请在 shell 中验证挂载隔离效果
```

进入容器内 bash 后：

```bash
echo $$                                      # 输出 1
ps -ef                                       # 只显示容器内的 bash、ps 等进程
mount | grep k8s_ns_mount_demo               # 能看到容器内挂载的 tmpfs
cat /tmp/k8s_ns_mount_demo/container_file.txt  # 能读到标记文件
ls -la /tmp/k8s_ns_mount_demo                # 能看到 container_file.txt
exit
```

容器运行期间，另开一个宿主机终端对比：

```bash
ls -A /tmp/k8s_ns_mount_demo                 # 空目录，看不到容器内的文件
mount | grep k8s_ns_mount_demo               # 没有输出，宿主机没有这个挂载
```

退出后父进程打印：

```text
[宿主机] 容器已退出
[宿主机] UTS 命名空间: uts:[4026531838]
[宿主机] IPC 命名空间: ipc:[4026531839]
[宿主机] PID 命名空间: pid:[4026531836]
[宿主机] Mount 命名空间: mnt:[4026531841]
[宿主机] hostname = "ubuntu"
[宿主机] 标记文件 /tmp/k8s_ns_mount_demo/container_file.txt 不存在（符合预期，tmpfs 仅在容器内可见）
```

## 验证要点

| 验证项 | 容器内 | 宿主机 |
|--------|--------|--------|
| `ps -ef` | 只有容器内进程 | 完整进程列表 |
| `echo $$` | 1 | 宿主机 shell 的真实 PID |
| tmpfs 挂载 | `mount` 可见 | 不可见 |
| 标记文件 | 可读 | 不存在 |
| mnt namespace 编号 | 与宿主机不同 | 原编号 |

> 实验结束后可执行 `rmdir /tmp/k8s_ns_mount_demo` 清理挂载点目录（正常情况下目录为空）。

## 与 Kubernetes 的对应关系

| 场景 | 行为 |
|------|------|
| 容器默认 | 每个容器有独立的 mount namespace 和 rootfs（overlayfs） |
| 同一 Pod 内容器 | **不共享** mount namespace，各自 rootfs 独立（与网络/UTS/IPC 不同） |
| Volume（emptyDir/PVC/ConfigMap/Secret/hostPath） | 以 bind mount 方式挂入各容器的 mount namespace |
| `/proc`、`/dev`、`/dev/shm`、`/etc/hosts`、`/etc/resolv.conf` | 由容器运行时在新的 mount namespace 中挂载 |
| `volumeMounts.mountPropagation` | 控制 hostPath/PVC 挂载事件是否向宿主机或其他容器传播 |
| Docker `-v` / `--mount` | 把宿主机目录 bind mount 进容器；`--shm-size` 控制 `/dev/shm` 大小 |

## 常见问题

| 现象 | 原因 |
|------|------|
| `clone: Operation not permitted` | 未使用 `sudo`，缺少 `CAP_SYS_ADMIN` |
| `mount(...): Permission denied` | 进程没有 `CAP_SYS_ADMIN`，或不在新的 mount namespace 中 |
| 宿主机 `/proc` 变成容器的视图 | 忘记执行 `MS_REC \| MS_PRIVATE`，挂载事件传播到了宿主机 |
| 容器内 `ps` 仍显示宿主机进程 | 没有重新挂载 `/proc`，或没有同时使用 `CLONE_NEWPID` |
| `mount(tmpfs)` 失败提示目录不存在 | 父进程创建挂载点失败，检查 `/tmp/k8s_ns_mount_demo` |
| 宿主机能看到标记文件 | 挂载传播未设为 private，或挂载点被错误地传播 |
| 容器退出后 tmpfs 文件消失 | mount namespace 销毁，属于正常现象 |
| 宿主机重启前出现残留挂载 | 检查 `findmnt /tmp/k8s_ns_mount_demo`，必要时 `umount` |

## 扩展实验：使用 unshare

```bash
# 创建独立的 mount namespace，并把传播设为私有
sudo unshare --mount --propagation private bash

mkdir -p /tmp/k8s_ns_mount_demo
mount -t tmpfs -o size=10m tmpfs /tmp/k8s_ns_mount_demo
mount | grep k8s_ns_mount_demo
exit

# 回到宿主机
mount | grep k8s_ns_mount_demo    # 没有输出
ls -A /tmp/k8s_ns_mount_demo      # 空目录
```

组合多个 namespace，复现本节效果：

```bash
sudo unshare --uts --ipc --pid --mount --fork --mount-proc bash

echo $$                       # 1
ps -ef                        # 只看到容器内进程
hostname container            # 在新的 UTS namespace 中修改主机名
hostname                      # 输出 container
exit
```

说明：

- `--mount-proc` 会自动挂载 procfs，并隐含 `--mount`
- `--fork` 必须加，否则 `unshare` 自身不会进入新的 PID namespace
- 显式加 `--propagation private`，避免挂载事件传播到宿主机

## 下一节

User namespace（`CLONE_NEWUSER`）：隔离用户和权限，使普通用户也能在自己的 namespace 内拥有 root 权限。
