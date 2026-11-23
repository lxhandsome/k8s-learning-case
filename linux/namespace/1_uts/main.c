#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>

/* 定义一个给 clone 用的栈，栈大小1M */
#define STACK_SIZE (1024 * 1024)
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

int container_main(void* arg)
{
    (void)arg;

    printf("\n[容器] 子进程已进入新的 UTS 命名空间，PID = %d\n", getpid());
    print_namespace("[容器]", "UTS 命名空间", "uts");
    print_hostname("[容器] 修改前");

    /* 设置新的 hostname，因为子进程在新的 UTS namespace 中，不影响宿主机 */
    if (sethostname("container", 10) == -1) {
        perror("sethostname");
    }
    print_hostname("[容器] 修改后");
    printf("[容器] 即将启动 /bin/bash，请在 shell 中执行 hostname 验证\n");
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
    print_hostname("[宿主机]");

    printf("\n[宿主机] 开始创建容器（CLONE_NEWUTS）...\n");
    fflush(stdout);
    int container_pid = clone(container_main, container_stack + STACK_SIZE,
                              CLONE_NEWUTS | SIGCHLD, NULL);
    if (container_pid == -1) {
        perror("clone");
        return 1;
    }
    printf("[宿主机] 容器进程在宿主机上的 PID = %d\n", container_pid);
    fflush(stdout);

    /* 等待子进程结束 */
    waitpid(container_pid, NULL, 0);

    printf("\n[宿主机] 容器已退出\n");
    print_namespace("[宿主机]", "UTS 命名空间", "uts");
    print_hostname("[宿主机]");
    return 0;
}
