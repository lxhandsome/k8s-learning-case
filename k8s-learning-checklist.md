# Kubernetes 系统学习清单

> **起点**：会用 kubectl 做基本操作
> **目标**：能独立部署、排障、运维中小规模集群
> **建议节奏**：每周 8–10 小时，全程约 13 周
> **官方文档（中文）**：https://kubernetes.io/zh-cn/docs/

## 怎么用这份清单

每个知识点都遵循同一个循环，别跳过任何一步：

1. **读概念**（Concepts）→ 知道它解决什么问题
2. **手写 YAML** → 不看教程从头写一遍
3. **故意弄坏** → 改错标签、删掉 PVC、配错探针
4. **修好它** → 用 `kubectl describe` / `logs` / `events` 定位
5. **写 3 行笔记** → 用你自己的话解释「它为什么存在」

> 完成一项就把 `[ ]` 改成 `[x]`。

## 进度总览

| # | 阶段 | 建议用时 | 开始前依赖 | 开始 | 完成 |
|---|---|---|---|---|---|
| 0 | 容器与 Linux 地基 | 1 周 | #5（Docker 组） | [ ] | [ ] |
| 1 | 核心对象与工作负载 | 2 周 | 无 | [ ] | [ ] |
| 2 | 网络与服务发现 | 2–3 周 | #4、#7（可选 #8、#9） | [ ] | [ ] |
| 3 | 存储 | 1 周 | #3 | [ ] | [ ] |
| 4 | 声明式 API / Operator / Helm | 2 周 | #1、#6 | [ ] | [ ] |
| 5 | 生产化：安全、可观测、弹性 | 3 周 | #2、#1（装监控栈用） | [ ] | [ ] |
| 6 | 集群原理与搭建 | 2 周 | #5、#6、#10 | [ ] | [ ] |
| 7 | 认证与生态（可选） | — | #1（可选 #9、#10） | [ ] | [ ] |

> 依赖编号对应下方「按阶段解锁的环境准备」，每个阶段开头也会重复列出。

---

# 你的本地实验环境（2026-11-16 实测）

> 这份清单已按这台机器的实际情况适配。换机器或重建集群后需要重新核对。

| 项 | 实际情况 |
|---|---|
| 集群类型 | kubeadm 单节点，context `kubernetes-admin@kubernetes` |
| 版本 | 控制面 **v1.35.3**，kubelet / kubeadm **v1.35.10** |
| 节点 | `k8s-master`，12 vCPU / 31 GiB / 451 GiB 磁盘，**无 control-plane 污点**（普通 Pod 可调度） |
| 容器运行时 | Docker 29.9.3 + **cri-dockerd**（CRI socket：`/var/run/cri-dockerd.sock`） |
| CNI | **flannel** ⚠️ 不支持 NetworkPolicy |
| kube-proxy | iptables 模式（默认） |
| etcd | 3.6.6，堆叠式静态 Pod `etcd-k8s-master` |
| 存储 | ❌ 没有 StorageClass |
| 缺失组件 | ❌ Ingress Controller、metrics-server、Helm、Gateway API CRDs、Prometheus、ArgoCD |
| 权限 | 当前用户不在 docker 组；`sudo` 需要密码；`crictl` / `etcdctl` 未配置 |

**结论**：阶段 0、1 可以直接开工；阶段 6 的 etcd 备份、证书、静态 Pod 也能直接做（从零重搭需另备 VM）；阶段 2、3、5 需要先补组件。

## 按阶段解锁的环境准备

> **这些不是「入学考试」，不用一次全装完。**
> Kubernetes 本体只负责调度和运行 Pod；Ingress、存储、指标、打包这些都属于「附加组件」，不在核心二进制里，需要单独安装。下面每一项都是某个阶段的**前置条件**，学到对应阶段之前装好即可。阶段 0、1 现在就能开始，什么都不用装。

| 准备项 | 何时需要 | 不装会怎样 | 性质 |
|---|---|---|---|
| #1 Helm | 阶段 4 | 无法打包 chart，装监控栈也依赖它 | 工具 |
| #2 metrics-server | 阶段 5 | `kubectl top` 报错，HPA 拿不到指标 | 学习对象 |
| #3 StorageClass | 阶段 3 | 所有 PVC 永远 Pending | 学习对象 |
| #4 Ingress Controller | 阶段 2 | Ingress 对象能创建，但没有任何流量进来 | 学习对象 |
| #5 Docker / crictl | 阶段 0（Docker）/ 阶段 6（crictl） | `docker ps` 权限拒绝，crictl 连不上运行时 | 工具 |
| #6 Go / etcdctl | 阶段 4（Go）/ 阶段 6（etcdctl） | kubebuilder 跑不起来，无法备份 etcd | 工具 |
| #7 Calico / kind | 阶段 2 | NetworkPolicy 写了不生效（最容易误以为学会了） | 学习对象 |
| #8 MetalLB | 阶段 2（可选） | LoadBalancer 类型 Service 永远 pending | 可选 |
| #9 Gateway API CRDs | 阶段 2 / 7（可选） | 无法创建 Gateway / HTTPRoute 对象 | 可选 |
| #10 VM 环境（multipass） | 阶段 6 / 7 | 无法从零搭三节点集群 | 工具 |

### 1. Helm（阶段 4 必需）

```bash
curl -fsSL https://raw.githubusercontent.com/helm/helm/main/scripts/get-helm-4 | bash
helm version
```

### 2. metrics-server（`kubectl top` 和 HPA 必需）

cri-dockerd 下 kubelet 使用自签证书，必须加 `--kubelet-insecure-tls`，否则 metrics-server 起不来：

```bash
kubectl apply -f https://github.com/kubernetes-sigs/metrics-server/releases/download/v0.9.1/components.yaml
kubectl -n kube-system patch deployment metrics-server --type=json \
  -p='[{"op":"add","path":"/spec/template/spec/containers/0/args/-","value":"--kubelet-insecure-tls"}]'
kubectl -n kube-system rollout status deployment/metrics-server
kubectl top nodes
```

### 3. StorageClass（阶段 3 必需）

```bash
kubectl apply -f https://raw.githubusercontent.com/rancher/local-path-provisioner/v0.0.37/deploy/local-path-storage.yaml
kubectl patch storageclass local-path \
  -p '{"metadata":{"annotations":{"storageclass.kubernetes.io/is-default-class":"true"}}}'
kubectl get storageclass
```

> local-path 只支持 `ReadWriteOnce`，数据落在节点 `/opt/local-path-provisioner`。单节点够用，但演示不了 `ReadWriteMany`。

### 4. Ingress Controller（阶段 2 必需）

```bash
kubectl apply -f https://raw.githubusercontent.com/kubernetes/ingress-nginx/controller-v1.15.1/deploy/static/provider/baremetal/deploy.yaml
kubectl -n ingress-nginx get pods -w
```

装好后通过 `http://192.168.31.193:<NodePort>` 访问。

### 5. Docker 与 crictl（阶段 0 用）

```bash
sudo usermod -aG docker $USER          # 重新登录后生效
sudo tee /etc/crictl.yaml >/dev/null <<'EOF'
runtime-endpoint: unix:///var/run/cri-dockerd.sock
image-endpoint: unix:///var/run/cri-dockerd.sock
timeout: 10
EOF
sudo crictl ps
```

### 6. Go 与 etcdctl（阶段 4 / 6 用）

```bash
# Go（kubebuilder 需要）
sudo snap install go --classic

# kubebuilder 脚手架
go install sigs.k8s.io/kubebuilder/v4@latest
echo 'export PATH=$PATH:$(go env GOPATH)/bin' >> ~/.bashrc

# etcdctl，版本要和集群 etcd 一致（你是 3.6.6）
curl -LO https://github.com/etcd-io/etcd/releases/download/v3.6.6/etcd-v3.6.6-linux-amd64.tar.gz
tar xzf etcd-v3.6.6-linux-amd64.tar.gz
sudo install -m 0755 etcd-v3.6.6-linux-amd64/etcdctl /usr/local/bin/
```

### 7. NetworkPolicy 练习环境（阶段 2 必需，二选一）

flannel 不执行 NetworkPolicy，有两个选择：

**方案 A（推荐，不动现有集群）：另起一个 kind 集群练**

```bash
# 先确保 docker 组已生效；kind 未安装的话：go install sigs.k8s.io/kind@latest
cat > kind-3node.yaml <<'EOF'
kind: Cluster
apiVersion: kind.x-k8s.io/v1alpha4
networking:
  disableDefaultCNI: true
  podSubnet: 10.244.0.0/16
nodes:
  - role: control-plane
  - role: worker
  - role: worker
EOF
kind create cluster --name lab --config kind-3node.yaml

# Calico 默认 IP 池是 192.168.0.0/16，必须改成和 podSubnet 一致
curl -LO https://raw.githubusercontent.com/projectcalico/calico/v3.33.1/manifests/calico.yaml
sed -i 's|# - name: CALICO_IPV4POOL_CIDR|- name: CALICO_IPV4POOL_CIDR|; s|#   value: "192.168.0.0/16"|  value: "10.244.0.0/16"|' calico.yaml
kubectl --context kind-lab apply -f calico.yaml
```

**方案 B（在当前集群用 Calico 替换 flannel）**

> ⚠️ 会让集群网络短暂中断。先备份 etcd 并确认能恢复。你当前 Pod 网段是 `10.244.0.0/16`，Calico 默认池是 `192.168.0.0/16`，必须改。

```bash
curl -LO https://raw.githubusercontent.com/projectcalico/calico/v3.33.1/manifests/calico.yaml
sed -i 's|# - name: CALICO_IPV4POOL_CIDR|- name: CALICO_IPV4POOL_CIDR|; s|#   value: "192.168.0.0/16"|  value: "10.244.0.0/16"|' calico.yaml
kubectl delete -f https://raw.githubusercontent.com/flannel-io/flannel/master/Documentation/kube-flannel.yml
kubectl apply -f calico.yaml
```

### 8.（可选）MetalLB

`hello-node` 的 LoadBalancer Service 一直是 `<pending>`，因为裸金属没有 LB 实现。装 MetalLB 后能拿到 EXTERNAL-IP：

```bash
kubectl apply -f https://raw.githubusercontent.com/metallb/metallb/v0.16.2/config/manifests/metallb-native.yaml
# 再创建 IPAddressPool + L2Advertisement，地址段选局域网里没人用的，例如 192.168.31.240-192.168.31.250
```

### 9.（可选）Gateway API CRDs

只装 CRD 只能学对象结构，要真正生效还需要实现（如 nginx-gateway-fabric / Cilium）：

```bash
kubectl apply -f https://github.com/kubernetes-sigs/gateway-api/releases/download/v1.7.0/standard-install.yaml
```

### 10. VM 环境（阶段 6 必需）

阶段 6 要从零搭三节点集群，需要 3 台虚拟机。你的机器支持 KVM（`/dev/kvm` 存在），用 multipass 最省事：

```bash
sudo snap install multipass
multipass launch 24.04 --name k8s-cp --cpus 2 --memory 4G --disk 20G
multipass launch 24.04 --name k8s-w1 --cpus 2 --memory 4G --disk 20G
multipass launch 24.04 --name k8s-w2 --cpus 2 --memory 4G --disk 20G
multipass list
```

> 宿主机 31 GiB 内存，3 台各 4 GiB 完全够。学完可以 `multipass stop --all` 省资源。
> 阶段 7 的多集群练习（Karmada / Cluster API）也复用这套 VM。

## 单节点集群做不了的练习

以下练习在单节点上「能跑但看不出效果」，统一放到 kind 三节点集群做：

- nodeAffinity / podAntiAffinity 的真实调度效果
- taint / toleration 的驱逐行为
- DaemonSet「每个节点一个」
- PDB + `kubectl drain`
- Node NotReady、节点失联故障演练
- 跨节点 Pod 网络与 CNI 排障

---

# 阶段 0：容器与 Linux 地基（1 周）

**目标**：知道 Pod 底下到底跑的是什么，为理解隔离、网络、资源限制打基础。
**说明**：这部分 k8s 官方文档较少，主要靠动手 + 系统文档；下方「补充阅读」按主题列了社区口碑最好的几篇。

> **开始前先完成**：准备 #5 的 Docker 部分（`sudo usermod -aG docker $USER` 后重新登录）。crictl 配置可以边学边做。

## 容器原理

- [ ] **namespace 隔离**：PID / NET / MNT / UTS / IPC / USER 各隔离什么 — [man7: namespaces(7)](https://man7.org/linux/man-pages/man7/namespaces.7.html)
- [ ] **cgroup v1 vs v2**：CPU、内存限制如何生效，为什么 k8s 在推 cgroup v2 — [Kubernetes: cgroups](https://kubernetes.io/zh-cn/docs/concepts/architecture/cgroups/)
- [ ] **镜像分层**：联合文件系统、CoW、多阶段构建 — [Docker: 多阶段构建](https://docs.docker.com/build/building/multi-stage/)
- [ ] **容器运行时**：dockerd / containerd / CRI-O 的关系，CRI 接口是什么 — [Kubernetes: 容器运行时接口 CRI（英文）](https://kubernetes.io/docs/concepts/architecture/cri/)
  > 你的集群正是 Docker + cri-dockerd 的组合，`/var/run/cri-dockerd.sock` 就是 kubelet 使用的 CRI socket。
- [ ] **dockershim 为什么被移除**（k8s 1.24）— [从 dockershim 迁移](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/migrating-from-dockershim/)

## Linux 网络

- [ ] **netns**：创建、进入、查看网络命名空间 — [man8: ip-netns](https://man7.org/linux/man-pages/man8/ip-netns.8.html)
- [ ] **veth pair**：一对虚拟网卡如何连接两个命名空间
- [ ] **网桥**：为什么需要 docker0 / cni0
- [ ] **iptables**：`nat` 表的 DNAT/SNAT 规则，理解后面 Service 的实现 — [iptables 教程](https://www.frozentux.net/iptables-tutorial/iptables-tutorial.html)
- [ ] **路由表与转发**：`ip route`、`net.ipv4.ip_forward`

## 阶段 0 补充阅读（按主题）

> 这些都是社区里公认质量高的材料，不是随便搜来的。建议每个主题先读一篇，再动手做对应的练习。文末附了读法建议。

### 总览：容器到底是什么

- **CoolShell《Docker 基础技术：Linux Namespace（上）》** — 陈皓的经典系列，从 chroot 讲到用 clone/unshare 手工造一个"容器" — [coolshell.org](https://coolshell.org/articles/17010.html/)
- **CoolShell《Docker 基础技术：Linux Namespace（下）》** — User / Network namespace 与 uid_map 映射 — [coolshell.org](https://coolshell.org/articles/17029.html)
- **CoolShell《Docker 基础技术：Linux CGroup》** — cgroup 四大功能，附手工限制 CPU 的完整实验 — [coolshell.org](https://coolshell.org/articles/17049.html)
- **CoolShell《Docker 基础技术：AUFS》** — 联合文件系统与镜像分层的由来 — [coolshell.org](https://coolshell.org/articles/17061.html)
- **Julia Evans《How Containers Work》**（英文，图解）— 把容器拆成 6 个 Linux 特性，附 15 行 bash 跑起一个容器 — [jvns.ca](https://jvns.ca/blog/2020/04/27/new-zine-how-containers-work/)

### 系统教材 / 高星仓库

- **《Docker 从入门到实践》第 12 章 底层实现**（GitHub **26k+ stars**）— 中文，覆盖架构、namespace、cgroups、UnionFS、容器格式、网络，每节都有可复现实验 — [GitHub](https://github.com/yeasy/docker_practice/blob/master/12_implementation/README.md) · [在线版](https://yeasy.gitbook.io/docker_practice/12_implementation/12.1_arch)
- **Liz Rice《containers-from-scratch》**（GitHub **2k+ stars**）— 用 Go 从零实现一个容器，最适合把 namespace / cgroup / chroot 串起来 — [GitHub](https://github.com/lizrice/containers-from-scratch)
- **bocker**（GitHub **12k+ stars**）— 用约 100 行 bash 实现 Docker，读源码能看清每条命令 — [GitHub](https://github.com/p8952/bocker)

### namespace / cgroup 动手

- 《Docker 从入门到实践》12.2 命名空间 — [在线版](https://yeasy.gitbook.io/docker_practice/12_implementation/12.2_namespace)
- 《Docker 从入门到实践》12.3 控制组 — [在线版](https://yeasy.gitbook.io/docker_practice/12_implementation/12.3_cgroups)

### 镜像与联合文件系统

- **《Overlay2 和 Device Mapper 实现原理》**（腾讯云）— lowerdir / upperdir / merged / workdir 四层结构与写时复制 — [腾讯云](https://cloud.tencent.com/developer/article/2470671)
- **Julia Evans《How containers work: overlayfs》**（英文，图解）— 用图讲清镜像分层 — [jvns.ca](https://jvns.ca/blog/2019/11/18/how-containers-work--overlayfs/)
- 《Docker 从入门到实践》12.4 联合文件系统 — [在线版](https://yeasy.gitbook.io/docker_practice/12_implementation/12.4_ufs)

### 网络：netns / veth / 网桥 / iptables

- **《从 Linux 内核到虚拟网络再到 K8s 容器网络的实现原理剖析》**（阿里云）— 手工创建 netns、veth、br0，逐步还原 Docker 网络，命令完整 — [阿里云](https://developer.aliyun.com/article/813025)
- **《跟唐老师学习云网络 - iptables - nat 穿越功能》**（华为云）— 看懂 DNAT / SNAT 与端口映射的数据包路径 — [华为云](https://bbs.huaweicloud.com/blogs/110460)
- 《Docker 从入门到实践》12.6 网络 — [在线版](https://yeasy.gitbook.io/docker_practice/12_implementation/12.6_network)

### 运行时：OCI / CRI / containerd

- **《Docker，containerd，CRI，CRI-O，OCI，runc 分不清？看这一篇就够了》**（腾讯云）— 把这几个名词的关系一次讲清 — [腾讯云](https://cloud.tencent.com/developer/article/1988350)
- **Kubernetes 官方 CRI 文档**（英文）— 接口定义与 kubelet 调用链 — [kubernetes.io](https://kubernetes.io/docs/concepts/architecture/cri/)

### 读法建议

1. **总览**先读 Julia Evans 或 CoolShell 上篇，建立「容器 = namespace + cgroup + rootfs + capabilities」的框架
2. 每读一个主题，立刻在机器上敲一遍对应的 `ip netns` / `cgroup` / `mount -t overlay` 命令
3. 读完运行时那篇，回集群执行 `sudo crictl info` 和 `kubectl describe node`，对照你机器上的 Docker + cri-dockerd

## 动手任务

- [ ] 用 `ip netns` 创建两个命名空间，用 veth pair + 网桥打通，互相 ping 通
- [ ] 用 `nsenter` 进入一个运行中的容器，查看它的网络栈
- [ ] 写一个多阶段构建的 Go/Java 镜像，体积压缩一半以上
- [ ] 用 `docker stats` + cgroup 文件观察容器内存限制被突破时的表现（OOM）
- [ ] 把当前用户加入 docker 组（`sudo usermod -aG docker $USER`），重新登录后验证 `docker ps`
- [ ] 按「准备 #5」配置 `/etc/crictl.yaml`，让 `sudo crictl ps` 能看到容器

**过关标准**：不看教程，能手工打通两个 netns 的通信，并解释沿途每一跳的规则。

---

# 阶段 1：核心对象与工作负载（2 周）

**目标**：所有资源 YAML 都能手写，理解每个控制器存在的理由。

> **开始前先完成**：无。集群已经可用，本阶段现在就能开始。

## 对象模型与 kubectl

- [ ] **Kubernetes 对象**：`spec` / `status` 的分离，期望状态 vs 实际状态 — [理解 Kubernetes 对象（英文）](https://kubernetes.io/docs/concepts/overview/working-with-objects/kubernetes-objects/)
- [ ] **声明式 vs 命令式管理**：`apply` 为什么比 `create` 重要 — [对象管理](https://kubernetes.io/zh-cn/docs/concepts/overview/working-with-objects/object-management/)
- [ ] **labels / selectors / annotations**：各自用途和区别 — [标签和选择算符](https://kubernetes.io/zh-cn/docs/concepts/overview/working-with-objects/labels/)
- [ ] **Namespace**：隔离边界在哪、什么资源不属于命名空间 — [命名空间](https://kubernetes.io/zh-cn/docs/concepts/overview/working-with-objects/namespaces/)
- [ ] **kubectl 进阶**：`-o jsonpath`、`--dry-run=client`、`kubectl explain`、`kubectl diff`、`diff -u <(kubectl get ...)` — [kubectl 快速参考](https://kubernetes.io/zh-cn/docs/reference/kubectl/quick-reference/)

## Pod

- [ ] **Pod 是什么**：为什么它是最小调度单位而不是容器 — [Pod](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/)
- [ ] **生命周期与状态**：Pending / Running / Succeeded / Failed / Unknown，以及 Waiting 状态下的 CrashLoopBackOff — [Pod 生命周期](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/pod-lifecycle/)
- [ ] **Init 容器**：串行启动、能和主容器共享什么 — [Init 容器](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/init-containers/)
- [ ] **Sidecar 容器**：原生 sidecar 与普通容器的区别 — [Sidecar 容器](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/sidecar-containers/)
- [ ] **三种探针**：liveness / readiness / startup 的触发条件和典型踩坑 — [配置存活/就绪/启动探针](https://kubernetes.io/zh-cn/docs/tasks/configure-pod-container/configure-liveness-readiness-startup-probes/)
- [ ] **资源请求与限制**：`requests` vs `limits`，CPU 和内存超限时的行为差异（CPU 限流 vs OOMKilled）— [容器资源管理](https://kubernetes.io/zh-cn/docs/concepts/configuration/manage-resources-containers/)
- [ ] **QoS 三档**：Guaranteed / Burstable / BestEffort 的判定规则与驱逐优先级 — [Pod 服务质量](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/pod-qos/)

## 工作负载控制器

- [ ] **ReplicaSet**：只负责维持副本数 — [ReplicaSet](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/replicaset/)
- [ ] **Deployment**：滚动更新 `maxSurge`/`maxUnavailable`、`rollout undo`、`revisionHistoryLimit` — [Deployment](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/deployment/)
- [ ] **StatefulSet**：稳定网络标识、有序启停、`volumeClaimTemplates` — [StatefulSet](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/statefulset/)
- [ ] **DaemonSet**：每个节点一个、节点加入时的行为 — [DaemonSet](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/daemonset/)
- [ ] **Job / CronJob**：`completions` / `parallelism` / `backoffLimit`、并发策略 — [Job](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/job/) · [CronJob](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/cron-jobs/)

## 调度

- [ ] **指定节点**：nodeSelector、nodeAffinity、podAffinity/antiAffinity — [将 Pod 指派到节点](https://kubernetes.io/zh-cn/docs/concepts/scheduling-eviction/assign-pod-node/)
- [ ] **污点与容忍**：污点三效果（NoSchedule/PreferNoSchedule/NoExecute）、系统污点 — [污点和容忍度](https://kubernetes.io/zh-cn/docs/concepts/scheduling-eviction/taint-and-toleration/)
- [ ] **优先级与抢占**：PriorityClass、高优先级 Pod 如何挤走低优先级 — [Pod 优先级与抢占](https://kubernetes.io/zh-cn/docs/concepts/scheduling-eviction/pod-priority-preemption/)
- [ ] **调度器视角**：过滤（Filter）→ 打分（Score）→ 绑定（Bind），调度失败看什么 — [kube-scheduler](https://kubernetes.io/zh-cn/docs/concepts/scheduling-eviction/kube-scheduler/)
  > ⚠️ 你是单节点，调度类练习只能验证语法；真实效果用 kind 三节点集群（见「单节点集群做不了的练习」）。

## 配置注入

- [ ] **ConfigMap**：环境变量 / 卷挂载 / `subPath`，热更新为什么有时不生效 — [ConfigMap](https://kubernetes.io/zh-cn/docs/concepts/configuration/configmap/)
- [ ] **Secret**：只是 base64 编码不是加密，`stringData`、`immutable` — [Secret](https://kubernetes.io/zh-cn/docs/concepts/configuration/secret/)
- [ ] **Downward API**：把自己 Pod 的元信息注入容器 — [Downward API](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/downward-api/)

## 动手任务

- [ ] 从零手写 YAML 部署「前端 + API + Postgres + Redis」，含探针、资源限制、ConfigMap、Secret
- [ ] 发布 v2 镜像，观察滚动更新过程，然后 `kubectl rollout undo` 回滚
- [ ] 故意把 readiness 探针配错，观察 Service 是否摘流量
- [ ] 设置一个低于实际内存需求的 limit，观察 OOMKilled 与重启计数
- [ ] 把 Pod 调度到有污点的节点上（先失败，再加 toleration 成功）

**过关标准**：关掉文档，20 分钟内写出一个生产可用的 Deployment + Service + ConfigMap + Secret。

---

# 阶段 2：网络与服务发现（2–3 周）

**目标**：能解释一个请求从外部进来、到 Pod 的完整路径，并独立排查网络故障。这是 k8s 最值钱的部分。

> **开始前先完成**：#4（ingress-nginx）、#7（NetworkPolicy 环境：kind + Calico，或替换 flannel）。
> **可选**：#8（MetalLB）、#9（Gateway API CRDs）。

## Service 与端点

- [ ] **Service 五种类型**：ClusterIP / NodePort / LoadBalancer / ExternalName / Headless — [Service](https://kubernetes.io/zh-cn/docs/concepts/services-networking/service/)
- [ ] **Endpoints 与 EndpointSlice**：为什么有 EndpointSlice — [EndpointSlice](https://kubernetes.io/zh-cn/docs/concepts/services-networking/endpoint-slices/)
- [ ] **虚拟 IP 与代理模式**：kube-proxy 的 iptables vs IPVS vs nftables，为什么 ClusterIP 是「假 IP」— [虚拟 IP 与 Service 代理](https://kubernetes.io/zh-cn/docs/reference/networking/virtual-ips/)

## DNS

- [ ] **集群内 DNS**：`svc.ns.svc.cluster.local` 的解析链路、`dnsPolicy`、`ndots` 陷阱 — [Service 与 Pod 的 DNS](https://kubernetes.io/zh-cn/docs/concepts/services-networking/dns-pod-service/)
- [ ] **自定义 DNS 服务**：CoreDNS 的 ConfigMap 配置 — [自定义 DNS 服务](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/dns-custom-nameservers/)

## 入口

- [ ] **Ingress**：host/path 路由、TLS、与 Service 的关系 — [Ingress](https://kubernetes.io/zh-cn/docs/concepts/services-networking/ingress/)
- [ ] **Ingress 控制器**：ingress-nginx 的工作原理 — [Ingress 控制器](https://kubernetes.io/zh-cn/docs/concepts/services-networking/ingress-controllers/)
- [ ] **Gateway API**：为什么它是 Ingress 的继任者（角色分离、类型化路由）— [Gateway API](https://kubernetes.io/zh-cn/docs/concepts/services-networking/gateway/)

## 网络模型与 CNI

- [ ] **Pod 网络模型**：每个 Pod 一个 IP、扁平网络、同 Pod 内容器共享网络栈 — [集群网络](https://kubernetes.io/zh-cn/docs/concepts/cluster-administration/networking/)
- [ ] **CNI 与网络插件**：CNI 只管什么，插件负责什么 — [网络插件](https://kubernetes.io/zh-cn/docs/concepts/extend-kubernetes/compute-storage-net/network-plugins/)
- [ ] **Calico**：BGP 模式 vs overlay（IPIP/VXLAN）— [Calico 文档](https://docs.tigera.io/calico/latest/about/)
- [ ] **Cilium**：eBPF 数据面为什么能替代 kube-proxy — [Cilium 文档](https://docs.cilium.io/en/stable/overview/intro/)

## NetworkPolicy

- [ ] **NetworkPolicy**：默认全通、ingress/egress 白名单、namespaceSelector/podSelector — [网络策略](https://kubernetes.io/zh-cn/docs/concepts/services-networking/network-policies/)
- [ ] ⚠️ **你的 CNI 是 flannel，NetworkPolicy 不会生效**。先按「准备 #7」安装 Calico 或另起 kind 集群，再做本项练习

## 动手任务

- [ ] 用 ingress-nginx 把应用暴露到集群外（节点 IP `192.168.31.193`），配一个 TLS 证书
- [ ] 写「默认拒绝 + 白名单」的 NetworkPolicy，让 A namespace 只能访问 B 的 5432 端口，并用 curl 验证
- [ ] 制造一次 DNS 解析失败（改 CoreDNS ConfigMap），用 `dig` / `nslookup` 定位并修复
- [ ] 删除 Service 的 selector 制造「无 Endpoint」故障，观察现象
- [ ] 用 `kubectl get endpointslices -o wide` 追踪一次请求的后端 Pod 列表
- [ ] （可选）装 MetalLB，把 `hello-node` 那个一直 pending 的 LoadBalancer Service 修好

**过关标准**：能画出「外部用户 → LB → Ingress → Service → Pod」的完整数据包路径，并说明每一步的 iptables/DNS 规则。

---

# 阶段 3：存储（1 周）

**目标**：理解 PV/PVC/StorageClass/CSI 的协作关系，能跑有状态应用。

> **开始前先完成**：#3（local-path StorageClass）。没装的话所有 PVC 会一直 Pending。

- [ ] **Volume 类型**：emptyDir / hostPath / configMap / secret / projected 的适用场景 — [卷](https://kubernetes.io/zh-cn/docs/concepts/storage/volumes/)
- [ ] **PV 与 PVC**：绑定机制、访问模式 RWO/ROX/RWX/RWOP 的真实含义 — [持久卷](https://kubernetes.io/zh-cn/docs/concepts/storage/persistent-volumes/)
- [ ] **StorageClass**：动态供给、`provisioner`、`parameters`、默认 StorageClass — [存储类](https://kubernetes.io/zh-cn/docs/concepts/storage/storage-classes/)
- [ ] **动态供给流程**：PVC → SC → CSI provisioner → 创建 PV → 绑定 — [动态卷供给](https://kubernetes.io/zh-cn/docs/concepts/storage/dynamic-provisioning/)
- [ ] **CSI**：为什么存储厂商只需要实现 CSI 驱动 — [CSI 卷](https://kubernetes.io/zh-cn/docs/concepts/storage/volumes/#csi)
- [ ] **StatefulSet + volumeClaimTemplates**：每个副本一个独立 PVC 的重建行为 — [StatefulSet](https://kubernetes.io/zh-cn/docs/concepts/workloads/controllers/statefulset/)
- [ ] **回收策略**：Delete vs Retain，怎么改 — [更改 PV 的回收策略](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/change-pv-reclaim-policy/)
- [ ] **卷快照**（了解）：VolumeSnapshot / VolumeSnapshotClass — [卷快照](https://kubernetes.io/zh-cn/docs/concepts/storage/volume-snapshots/)

## 动手任务

- [ ] 部署 Postgres StatefulSet + 动态 PVC，写入数据后删 Pod、删 StatefulSet 再重建，验证数据仍在
- [ ] 把 PV 回收策略改成 Retain，删除 PVC 后手工恢复数据
- [ ] 配置一个使用 `subPath` 挂载 ConfigMap 单文件的 Pod，观察 ConfigMap 更新后文件是否变化

**过关标准**：能解释「PVC 为什么是 Pending」，并能区分是 SC 不存在、容量不足还是访问模式不匹配。

---

# 阶段 4：声明式 API / Operator / Helm（2 周）

**目标**：真正理解 k8s 的「控制器模式」，能扩展 API。

> **开始前先完成**：#1（Helm）、#6（Go + kubebuilder 脚手架）。Kustomize 随 kubectl 自带，不用装。

## 原理

- [ ] **控制器模式**：reconcile 循环、调谐循环、期望状态收敛 — [控制器](https://kubernetes.io/zh-cn/docs/concepts/architecture/controller/)
- [ ] **API 概念**：资源/动词、watch、server-side apply、资源版本 — [API 概念](https://kubernetes.io/zh-cn/docs/reference/using-api/api-concepts/)
- [ ] **informer 机制**：list-watch、本地缓存、workqueue、为什么不用轮询 — [client-go 示例](https://github.com/kubernetes/sample-controller)

## 扩展

- [ ] **CRD**：自定义资源定义、schema、`kubectl get` 如何识别新类型 — [自定义资源](https://kubernetes.io/zh-cn/docs/concepts/extend-kubernetes/api-extension/custom-resources/)
- [ ] **Operator**：CRD + 控制器的组合模式，什么场景该写 Operator — [Operator 模式](https://kubernetes.io/zh-cn/docs/concepts/extend-kubernetes/operator/)
- [ ] **kubebuilder**：脚手架、controller-runtime、reconcile 函数 — [Kubebuilder Book](https://book.kubebuilder.io/)
  > 需要 Go 和 kubebuilder 脚手架，本机目前没有 Go，见「准备 #6」。

## 打包与部署

- [ ] **Helm**：chart 结构、模板函数、values 覆盖、release 管理、`helm diff` / `rollback` — [Helm 文档（中文）](https://helm.sh/zh/docs/)
  > 机器上还没装 Helm，先完成「准备 #1」。
- [ ] **Helm Chart 模板**：`_helpers.tpl`、`values.yaml` 设计 — [Chart 模板指南](https://helm.sh/zh/docs/chart_template_guide/)
- [ ] **Kustomize**：base/overlay 多环境管理，与 Helm 的取舍 — [Kustomize](https://kubernetes.io/zh-cn/docs/tasks/manage-kubernetes-objects/kustomization/)

## 动手任务

- [ ] 用 kubebuilder 写一个最小 Operator：监听自定义资源，自动创建/删除 Deployment
- [ ] 把阶段 1 的应用打成 Helm chart，用 values 区分 dev/prod
- [ ] 用 Kustomize 管理同一应用的 staging/production 两套配置
- [ ] 用 `kubectl get --raw` 直接访问 API，观察 watch 流的输出

**过关标准**：能讲清「kubectl apply 之后到 Pod 真正跑起来」中间经过的所有组件和队列。

---

# 阶段 5：生产化：安全、可观测、弹性（3 周）

**目标**：达到能碰生产环境的水平。

> **开始前先完成**：#2（metrics-server）、#1（Helm）；再按文末附录装 kube-prometheus-stack。
> **需要多节点**：PDB + drain 练习用 #7 的 kind 三节点集群。

## 认证与授权

- [ ] **认证**：证书、ServiceAccount token、OIDC，kubeconfig 的结构 — [认证](https://kubernetes.io/zh-cn/docs/reference/access-authn-authz/authentication/)
- [ ] **授权**：RBAC 的 Role/ClusterRole/Binding、verbs/resources/apiGroups — [RBAC 鉴权](https://kubernetes.io/zh-cn/docs/reference/access-authn-authz/rbac/)
- [ ] **ServiceAccount**：与普通用户的区别、自动挂载、`automountServiceAccountToken` — [配置 ServiceAccount](https://kubernetes.io/zh-cn/docs/tasks/configure-pod-container/configure-service-account/)
- [ ] **Kubernetes API 访问控制**：认证 → 授权 → 准入 三道关 — [API 访问控制](https://kubernetes.io/zh-cn/docs/concepts/security/controlling-access/)

## 准入与安全

- [ ] **准入控制器**：内置准入控制器列表、执行顺序 — [准入控制器](https://kubernetes.io/zh-cn/docs/reference/access-authn-authz/admission-controllers/)
- [ ] **准入 Webhook**：validating / mutating webhook 的工作方式 — [动态准入控制](https://kubernetes.io/zh-cn/docs/reference/access-authn-authz/extensible-admission-controllers/)
- [ ] **Pod 安全标准 PSS**：privileged / baseline / restricted 三档 — [Pod 安全标准](https://kubernetes.io/zh-cn/docs/concepts/security/pod-security-standards/)
- [ ] **Pod 安全准入 PSA**：怎么用 namespace 标签启用 — [Pod 安全准入](https://kubernetes.io/zh-cn/docs/concepts/security/pod-security-admission/)
- [ ] **securityContext**：runAsUser、capabilities、readOnlyRootFilesystem — [安全上下文](https://kubernetes.io/zh-cn/docs/tasks/configure-pod-container/security-context/)
- [ ] **Secret 最佳实践**：静态加密、外部密钥管理（Vault / External Secrets）— [Secret 良好实践](https://kubernetes.io/zh-cn/docs/concepts/security/secrets-good-practices/)
- [ ] **策略引擎**（了解）：OPA Gatekeeper / Kyverno — [Kyverno 文档](https://kyverno.io/docs/)

## 可观测性

- [ ] **日志架构**：节点级日志、sidecar 模式、日志轮转 — [日志架构](https://kubernetes.io/zh-cn/docs/concepts/cluster-administration/logging/)
- [ ] **指标管线**：metrics-server 如何提供 `kubectl top` — [资源指标管道](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-cluster/resource-metrics-pipeline/)
- [ ] **系统指标**：kubelet/cAdvisor 暴露什么、有哪些关键指标 — [系统指标](https://kubernetes.io/zh-cn/docs/concepts/cluster-administration/system-metrics/)
- [ ] **Prometheus**：抓取配置、ServiceMonitor、PromQL 基础 — [Prometheus 文档](https://prometheus.io/docs/introduction/overview/)
- [ ] **日志方案**：Loki 或 EFK 的选型与架构 — [Loki 文档](https://grafana.com/docs/loki/latest/)
- [ ] **告警**：Node NotReady、Pod 重启、PVC 使用率、apiserver 延迟、证书过期

## 弹性与容量

- [ ] **HPA**：依赖 metrics-server、CPU 指标延迟、自定义指标 — [水平 Pod 自动扩缩](https://kubernetes.io/zh-cn/docs/concepts/workloads/autoscaling/horizontal-pod-autoscale/)
  > 你的集群还没装 metrics-server（现在 `kubectl top` 会报 Metrics API not available），先做「准备 #2」。
- [ ] **HPA 实践**：`kubectl autoscale`、扩缩容冷却窗口 — [HPA 演练](https://kubernetes.io/zh-cn/docs/tasks/run-application/horizontal-pod-autoscale-walkthrough/)
- [ ] **VPA**：为什么改资源要重建 Pod — [VPA 文档](https://github.com/kubernetes/autoscaler/tree/master/vertical-pod-autoscaler)
- [ ] **PDB**：`minAvailable` / `maxUnavailable`，保护滚动更新与节点维护 — [干扰与 PDB](https://kubernetes.io/zh-cn/docs/concepts/workloads/pods/disruptions/)
  > 单节点 drain 没有接收方，这个练习放到 kind 三节点集群做。
- [ ] **ResourceQuota / LimitRange**：namespace 级配额限制 — [资源配额](https://kubernetes.io/zh-cn/docs/concepts/policy/resource-quotas/) · [LimitRange](https://kubernetes.io/zh-cn/docs/concepts/policy/limit-range/)
- [ ] **节点压力驱逐**：memory.available、nodefs 等阈值 — [节点压力驱逐](https://kubernetes.io/zh-cn/docs/concepts/scheduling-eviction/node-pressure-eviction/)
- [ ] **Cluster Autoscaler**（云上）：节点池扩容触发条件 — [Cluster Autoscaler](https://github.com/kubernetes/autoscaler/tree/master/cluster-autoscaler)

## 发布策略

- [ ] **滚动更新**（已学）+ **金丝雀 / 蓝绿**：Argo Rollouts 或 Flagger — [Argo Rollouts](https://github.com/argoproj/argo-rollouts)
- [ ] **审计日志**：记录谁做了什么 — [审计](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-cluster/audit/)

## 动手任务

- [ ] 搭 Prometheus + Grafana，配置 3 条有效告警并触发验证
- [ ] 建一个只读 RBAC 用户，用 `kubectl auth can-i --as` 验证越权被拒
- [ ] 用 k6 / hey 压测，观察 HPA 扩容和缩容冷却
- [ ] 给应用启用 `restricted` 级别的 PSA，修掉所有不合规配置
- [ ] 配置 PDB，然后 `kubectl drain` 节点，观察 Pod 迁移是否被保护

**过关标准**：能独立为一个新应用配齐 RBAC、资源配额、探针、PDB、HPA、监控告警。

---

# 阶段 6：集群原理与搭建（2 周）

**目标**：能自己搭集群、升级、备份，并在控制面出问题时定位。

> **开始前先完成**：#5（crictl）、#6（etcdctl）、#10（multipass 三台 VM）。
> 现有集群可以直接练 etcd 快照、证书检查、静态 Pod 观察；**从零搭建请用 VM，不要 reset 当前集群**。

## 架构

- [ ] **组件职责**：apiserver（唯一读写 etcd 的入口）、etcd、scheduler、controller-manager、kubelet、kube-proxy — [Kubernetes 组件](https://kubernetes.io/zh-cn/docs/concepts/overview/components/)
- [ ] **节点**：节点注册、心跳、状态条件 — [节点](https://kubernetes.io/zh-cn/docs/concepts/architecture/nodes/)
- [ ] **控制面到节点的通信**：apiserver → kubelet、apiserver → etcd 的 TLS 路径 — [控制面与节点通信](https://kubernetes.io/zh-cn/docs/concepts/architecture/control-plane-node-communication/)
- [ ] **静态 Pod**：`/etc/kubernetes/manifests` 里的控制面组件如何被 kubelet 拉起
- [ ] **kubelet**：证书轮换、`crictl` 与 kubelet 日志的关系 — [kubelet 参考](https://kubernetes.io/zh-cn/docs/reference/command-line-tools-reference/kubelet/)

## 搭建与运维

- [ ] **kubeadm 搭集群**：三节点（1 控制面 + 2 工作节点），指定 Pod 网段、安装 CNI — [使用 kubeadm 创建集群](https://kubernetes.io/zh-cn/docs/setup/production-environment/tools/kubeadm/create-cluster-kubeadm/)
  > 你已经有一个 kubeadm 单节点集群。从零重搭请用 multipass / VM 另起 3 台，**不要 `kubeadm reset` 当前集群**。
- [ ] **集群升级**：控制面 → 工作节点，版本偏差规则 — [升级 kubeadm 集群](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/kubeadm/kubeadm-upgrade/)
  > 你的集群已经存在偏差：控制面 v1.35.3 / kubelet v1.35.10。升级前先 `kubectl version` 确认目标版本，并做 etcd 快照。
- [ ] **证书管理**：CA、各组件证书、`kubeadm certs check-expiration`、自动轮换 — [kubeadm 证书管理](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/kubeadm/kubeadm-certs/)
- [ ] **etcd 运维**：`etcdctl snapshot save/restore`、defrag、健康检查 — [etcd 集群运维](https://kubernetes.io/zh-cn/docs/tasks/administer-cluster/configure-upgrade-etcd/)
  > 本机 etcd 是 3.6.6 且没有 etcdctl；也可以直接 `kubectl exec -n kube-system etcd-k8s-master -- etcdctl ...` 用 Pod 里的二进制。
- [ ] **高可用拓扑**：堆叠 etcd vs 外部 etcd、多控制面 + 负载均衡 — [高可用拓扑](https://kubernetes.io/zh-cn/docs/setup/production-environment/tools/kubeadm/ha-topology/)

## 排障

- [ ] **应用排障**：`describe` / `logs` / `events` 三板斧 — [调试 Pod](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-application/debug-pods/)
- [ ] **Service 排障**：按 DNS → Endpoint → 网络策略 → iptables 的顺序 — [调试 Service](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-application/debug-service/)
- [ ] **kubectl debug**：临时容器、调试镜像、节点调试 — [调试运行中的 Pod](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-application/debug-running-pod/)
- [ ] **集群排障**：节点 NotReady、apiserver 不可用、etcd 慢 — [集群故障排查](https://kubernetes.io/zh-cn/docs/tasks/debug/debug-cluster/)
- [ ] **Kubernetes The Hard Way**：从二进制开始手工搭一遍（不跑起来也要读完）— [KTHW](https://github.com/kelseyhightower/kubernetes-the-hard-way)

## 动手任务

- [ ] kubeadm 从零搭三节点集群，全程不看教程
- [ ] 做一次 minor 版本升级
- [ ] etcd 快照备份 → 删数据 → 恢复 → 集群正常
- [ ] 关掉一个控制面节点，验证集群仍可用
- [ ] 用 `crictl ps` + `journalctl -u kubelet` 定位一次 Pod 启动失败

**过关标准**：生产集群的搭建、升级、备份恢复，全部能独立完成。

---

# 阶段 7：认证与生态（可选）

> **开始前先完成**：#1（Helm，装 ArgoCD 用）。其余按兴趣选：#9（Gateway API）、#10（多集群练习的 VM）。

- [ ] **CKA 认证**：纯实操 2 小时，是检验手速和排障能力的最好标尺 — [CKA 官方页面](https://training.linuxfoundation.org/certification/certified-kubernetes-administrator-cka/)
- [ ] **CKAD / CKS**：偏应用开发 / 偏安全，按需选择
- [ ] **GitOps**：ArgoCD 或 Flux，声明式发布 + 漂移检测 — [Argo CD 文档](https://argo-cd.readthedocs.io/)
- [ ] **Service Mesh**：Istio 的 sidecar 与 ambient 模式 — [Istio 中文文档](https://istio.io/latest/zh/docs/)
- [ ] **Gateway API**：作为 Ingress 的继任者深入 — [Gateway API 官方](https://gateway-api.sigs.k8s.io/)
- [ ] **多集群**：Karmada / Cluster API — [Cluster API](https://cluster-api.sigs.k8s.io/)
- [ ] **事件驱动扩缩容**（了解）：KEDA

---

# 必做的 4 个实战项目

- [ ] **1. 完整三层应用**：前端 + API + Postgres + Redis，含 Ingress、PVC、探针、HPA、NetworkPolicy、Helm chart
- [ ] **2. 自建集群**：kubeadm 三节点集群 + 升级 + etcd 恢复
- [ ] **3. 故障演练**：见下方清单，每项都自己制造并修复
- [ ] **4. GitOps 发布**：改 Git 里的镜像 tag，ArgoCD 自动同步；手动改集群后被自动纠正

# 故障演练清单（每项都要能 10 分钟内定位）

- [ ] Pod 一直 `Pending` —— 资源不足 / 污点 / 亲和性冲突
- [ ] Pod 反复 `CrashLoopBackOff` —— 看 `logs --previous`
- [ ] Pod `ImagePullBackOff` —— 镜像名 / 私有仓库 secret
- [ ] Pod `Running` 但 `0/1 Ready` —— readiness 探针失败
- [ ] Pod `OOMKilled` —— limit 小于实际需求
- [ ] Service 访问不通但 Pod 正常 —— selector 不匹配导致无 Endpoint
- [ ] 集群内 DNS 解析失败 —— CoreDNS / dnsPolicy
- [ ] 跨节点 Pod 不通 —— CNI 故障 / NetworkPolicy 拦截
- [ ] PVC 一直 `Pending` —— SC 不存在 / 容量 / 访问模式
- [ ] 节点 `NotReady` —— kubelet 挂 / 磁盘压力 / 网络
- [ ] apiserver 响应慢 —— etcd 慢 / 大量 watch / 负载
- [ ] Ingress 404 或 502 —— backend 名称 / 端口 / 就绪状态

---

# 附录：阶段 5 / 7 的组件安装

## Prometheus + Grafana（阶段 5）

```bash
helm repo add prometheus-community https://prometheus-community.github.io/helm-charts
helm repo update
helm install kps prometheus-community/kube-prometheus-stack -n monitoring --create-namespace
kubectl -n monitoring get pods
```

占用约 2–3 GiB 内存，31 GiB 的机器完全够。Grafana 默认用户 `admin`，密码用：

```bash
kubectl -n monitoring get secret kps-grafana -o jsonpath='{.data.admin-password}' | base64 -d
```

## Argo CD（阶段 7）

```bash
kubectl create namespace argocd
kubectl apply -n argocd -f https://raw.githubusercontent.com/argoproj/argo-cd/v3.6.1/manifests/install.yaml
kubectl -n argocd get pods
```

---

# 推荐资源

| 资源 | 用途 |
|---|---|
| [Kubernetes 官方中文文档](https://kubernetes.io/zh-cn/docs/) | 唯一权威主线，精读 Concepts，跟着 Tasks 敲 |
| [极客时间《深入剖析 Kubernetes》张磊](https://time.geekbang.org/column/intro/116) | 中文讲原理最好的，CRI/CNI/CSI 与控制器模式 |
| [Kubernetes The Hard Way](https://github.com/kelseyhightower/kubernetes-the-hard-way) | 阶段 6 必做 |
| [Kubernetes Up & Running（第 3 版）](https://www.oreilly.com/library/view/kubernetes-up-and/9781098110192/) | 阶段 1–3 辅助读物 |
| [Production Kubernetes](https://www.oreilly.com/library/view/production-kubernetes/9781492056737/) | 阶段 5 生产实践 |
| [kubectl 快速参考](https://kubernetes.io/zh-cn/docs/reference/kubectl/quick-reference/) | 随手查 |
| `kubectl explain <资源> --recursive` | 比任何教程都准的字段手册 |
| [Killercoda K8s 场景](https://killercoda.com/kubernetes) | 免安装练习环境 |

# 三个避坑

1. **只看视频不动手**：每个概念必须配一个能跑起来的 YAML，外加一次「故意弄坏再修好」。
2. **过早学生态**：Istio、Operator、Karmada 都是阶段 4–5 之后的事。
3. **不练排障**：会部署的人很多，能 10 分钟内定位 Service 为什么没流量的人很少 —— 这是分水岭。
