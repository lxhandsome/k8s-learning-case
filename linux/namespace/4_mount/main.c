#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* 定义一个给 clone 用的栈，栈大小1M */
#define STACK_SIZE (1024 * 1024)
/* 容器内 tmpfs 的挂载点，由父进程提前创建 */
#define MOUNT_POINT "/tmp/k8s_ns_mount_demo"
/* 容器内写入的标记文件，用于验证宿主机不可见 */
#define MARKER_FILE MOUNT_POINT "/container_file.txt"

static char container_stack[STACK_SIZE];

char* const container_args[] = {
    "/bin/bash",
    NULL
};

/* 打印当前进程所属的 namespace 编号，便于对比宿主机与容器是否隔离 */
static void print_namespace(const char *tag, const char *label, const char *ns_name)
{
    char path[64];
    char ns_id[128];

    snprintf(path, sizeof(path), "/proc/self/ns/%s", ns_name);
    ssize_t len = readlink(path, ns_id, sizeof(ns_id) - 1);
    if (len == -1) {
        printf("%s %s: 读取失败\n", tag, label);
        return;
    }
    ns_id[len] = '\0';
    printf("%s %s: %s\n", tag, label, ns_id);
}

/* 打印当前进程的 hostname */
static void print_hostname(const char *tag)
{
    char hostname[256] = {0};

    if (gethostname(hostname, sizeof(hostname) - 1) == -1) {
        perror("gethostname");
        return;
    }
    printf("%s hostname = \"%s\"\n", tag, hostname);
}

/*
 * 在容器内准备 mount namespace：
 * 1. 把挂载传播设为私有，防止容器内的挂载事件传播回宿主机
 * 2. 重新挂载 /proc，让 ps 看到新 PID namespace 的进程视图
 * 3. 挂载 tmpfs 并写入文件，演示文件系统隔离
 */
static int setup_mount_namespace(void)
{
    /* 1. 递归地把本 namespace 内所有挂载点设置为私有传播 */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == -1) {
        perror("[容器] mount(MS_REC | MS_PRIVATE)");
        return -1;
    }
    printf("[容器] 挂载传播已设为私有（MS_PRIVATE），容器内的挂载不会影响宿主机\n");

    /* 2. 重新挂载 /proc：procfs 的进程视图由挂载它的 PID namespace 决定 */
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == -1) {
        perror("[容器] mount(/proc)");
        return -1;
    }
    printf("[容器] 已重新挂载 /proc，ps 将只显示本 PID namespace 内的进程\n");

    /* 3. 挂载 tmpfs，演示容器内文件系统与宿主机隔离 */
    if (mount("tmpfs", MOUNT_POINT, "tmpfs", MS_NOSUID | MS_NODEV, "size=10m") == -1) {
        perror("[容器] mount(tmpfs)");
        return -1;
    }
    printf("[容器] 已在 %s 挂载 tmpfs（大小 10MB）\n", MOUNT_POINT);

    /* 在 tmpfs 中写入标记文件 */
    FILE *fp = fopen(MARKER_FILE, "w");
    if (fp == NULL) {
        perror("[容器] fopen");
        return -1;
    }
    fprintf(fp, "这个文件只存在于容器的 mount namespace 中\n");
    fclose(fp);
    printf("[容器] 已写入标记文件 %s\n", MARKER_FILE);

    return 0;
}

int container_main(void* arg)
{
    (void)arg;

    printf("\n[容器] 子进程已进入新的 UTS + IPC + PID + Mount 命名空间\n");
    printf("[容器] 子进程 PID = %d（新 PID 命名空间中的 init 进程）\n", getpid());
    print_namespace("[容器]", "UTS 命名空间", "uts");
    print_namespace("[容器]", "IPC 命名空间", "ipc");
    print_namespace("[容器]", "PID 命名空间", "pid");
    print_namespace("[容器]", "Mount 命名空间", "mnt");
    print_hostname("[容器] 修改前");

    /* 设置新的 hostname，因为子进程在新的 UTS namespace 中，不影响宿主机 */
    if (sethostname("container", 10) == -1) {
        perror("sethostname");
    }
    print_hostname("[容器] 修改后");

    /* 准备文件系统隔离 */
    if (setup_mount_namespace() == -1) {
        printf("[容器] mount namespace 初始化失败，容器退出\n");
        return 1;
    }

    printf("[容器] 提示：执行 ps -ef 只会看到容器内的进程\n");
    printf("[容器] 提示：执行 mount | grep k8s_ns_mount_demo 可查看 tmpfs\n");
    printf("[容器] 提示：执行 cat %s 可读取标记文件\n", MARKER_FILE);
    printf("[容器] 即将启动 /bin/bash，请在 shell 中验证挂载隔离效果\n");
    fflush(stdout);

    /* 直接执行一个 shell，以便观察进程空间里的资源是否被隔离 */
    execv(container_args[0], container_args);
    perror("execv");
    printf("[容器] 启动 /bin/bash 失败\n");
    return 1;
}

int main()
{
    printf("\n[宿主机] 父进程 PID = %d\n", getpid());
    print_namespace("[宿主机]", "UTS 命名空间", "uts");
    print_namespace("[宿主机]", "IPC 命名空间", "ipc");
    print_namespace("[宿主机]", "PID 命名空间", "pid");
    print_namespace("[宿主机]", "Mount 命名空间", "mnt");
    print_hostname("[宿主机]");

    /* 创建 tmpfs 挂载点；容器内的挂载仅存在于容器自己的 mount namespace */
    if (mkdir(MOUNT_POINT, 0755) == -1 && errno != EEXIST) {
        perror("mkdir");
        return 1;
    }
    printf("[宿主机] 容器挂载点：%s（宿主机上保持为空目录）\n", MOUNT_POINT);

    printf("\n[宿主机] 开始创建容器（CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS）...\n");
    fflush(stdout);
    int container_pid = clone(container_main, container_stack + STACK_SIZE,
                              CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                              NULL);
    if (container_pid == -1) {
        perror("clone");
        return 1;
    }
    printf("[宿主机] 容器进程在宿主机上的 PID = %d（容器内看到的是 1）\n", container_pid);
    fflush(stdout);

    /* 等待子进程结束 */
    waitpid(container_pid, NULL, 0);

    printf("\n[宿主机] 容器已退出\n");
    print_namespace("[宿主机]", "UTS 命名空间", "uts");
    print_namespace("[宿主机]", "IPC 命名空间", "ipc");
    print_namespace("[宿主机]", "PID 命名空间", "pid");
    print_namespace("[宿主机]", "Mount 命名空间", "mnt");
    print_hostname("[宿主机]");

    /* tmpfs 随 mount namespace 销毁，标记文件不应在宿主机上出现 */
    if (access(MARKER_FILE, F_OK) == -1) {
        printf("[宿主机] 标记文件 %s 不存在（符合预期，tmpfs 仅在容器内可见）\n", MARKER_FILE);
    } else {
        printf("[宿主机] 警告：标记文件 %s 在宿主机可见，挂载隔离可能失败\n", MARKER_FILE);
    }

    return 0;
}
