# Linux 智能物联网网关

面向弱网环境的边缘网关：向下采集设备数据，向上通过 MQTT 上报云端，支持断网缓存、
远程升级与失败回滚。

**已在实际硬件验证**：正点原子 i.MX6ULL 阿尔法（Cortex-A7 800MHz / 512MB / eMMC 8GB /
Linux 4.1.15 / glibc 2.23），数据经局域网 MQTT 上报到 PC 端 broker。

全部代码自行实现，**不依赖任何第三方库**：MQTT 协议客户端、HTTP 下载器、SHA256 算法
均为项目内实现。

---

## 架构

```
hal_sim / hal_uart        硬件层：唯一与具体设备绑定的地方
        │
        ▼
     ringbuf              单生产/单消费无锁环形缓冲，采集与处理解耦
        │
        ▼
      frame               帧解析：粘包、半包、校验失败重同步
        │
        ▼
      main                线程编排
        │
        ├──► mqtt         手写 MQTT 3.1.1，上行数据
        └──► store        断网时落盘，恢复后补发
```

换硬件只改 `src/hal_uart.c`，业务逻辑一行不动。

## 目录

```
gateway/
├── CMakeLists.txt
├── run.sh                      守护脚本：启动、失败回滚、崩溃重启
├── cmake/toolchain-arm.cmake   交叉编译配置
├── include/   hal.h  ringbuf.h  frame.h  mqtt.h  store.h  sha256.h  http.h  ota.h
├── src/       main.c  hal_sim.c  hal_uart.c  ringbuf.c  frame.c
│              mqtt.c  store.c  sha256.c  http.c  ota.c
└── tests/     test_ringbuf.c
```

---

## 编译

### x86（开发调试）

```bash
cmake -B build
cmake --build build -j4
```

### ARM（交叉编译上板）

需要 `arm-linux-gnueabihf-` 工具链（gcc 4.9.4，glibc 2.23，与板子匹配）：

```bash
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm.cmake
cmake --build build-arm -j4
file build-arm/gateway      # 必须是 ELF 32-bit LSB executable, ARM
```

> 不要用 `apt install gcc-arm-linux-gnueabihf`：那是 gcc 11，链接 glibc 2.35，
> 在板子上会报 `GLIBC_2.34 not found`。

---

## 运行

```bash
./build/gateway                        # 模拟数据源，本地打印
./build/gateway sim 127.0.0.1          # 上报到本机 MQTT broker
./build/gateway sim 192.168.0.112      # 上报到局域网 broker
./build/gateway uart /dev/ttymxc2      # 从真实串口采集
```

第三个参数可指定主题：`./build/gateway sim 192.168.0.112 my/topic`

订阅端：

```bash
mosquitto_sub -h 192.168.0.112 -t "gw/data" -v
```

---

## 功能

### 1. 采集与帧解析

模拟设备每 200ms 产生一帧 12 字节数据：

```
AA 55 | seq(2) | temp(2) | hum(2) | volt(2) | crc(2)     大端
```

解析用状态机逐字节找帧头，凑满 12 字节后校验；**校验失败只丢 1 个字节重新同步**，
不会把后面正常的数据一起扔掉。粘包、半包都能正确处理。

### 2. MQTT 3.1.1 客户端

自行拼报文，实现 CONNECT、CONNACK、PUBLISH(QoS0)、PINGREQ/PINGRESP。

- 剩余长度用 MQTT 的变长编码
- 空闲超过 keepalive/2 发 PINGREQ，避免被 broker 断开
- **线程约定**：所有调用在同一线程内，内部不加锁

### 3. 断网缓存

```
queue/
├── seg_000000.log      每段 1MB，写满自动切段
└── pos                 读位点 "段号 偏移"
```

每条记录 `[len:2][crc:2][data:len]`。

| 问题 | 做法 |
|---|---|
| 写一半断电，尾部有半条记录 | 启动时扫描当前段，长度/校验对不上就 `ftruncate` 截断 |
| 必须先发出去才能删 | 位点只在 `store_advance()` 推进，写完立即 `fsync` + 原子 `rename` |
| 缓存无限增长撑爆 eMMC | 1MB 一段，整段消费完直接 `unlink` |

实测（i.MX6ULL 板上）：断开 broker 5 分钟，缓存写入 74KB / 约 1500 条；恢复后
订阅端共收到 1543 条（积压 1503 + 补发期间实时 40），无丢失、无乱序。

资源占用：交叉编译后二进制 33KB，运行时常驻内存 384KB（`VmRSS`）。

### 4. OTA

```
app/
├── v1/gateway     旧版本，升级过程中绝不触碰
├── v2/gateway     新版本写这里
└── current -> v1  符号链接，启动走这里
```

流程：下载 → SHA256 校验 → 写入新目录 → 原子 `rename` 切换链接 → 重启 → 确认 → 失败回滚。

```bash
./build/gateway ota http://127.0.0.1:8000/pkg.bin <sha256>   # 下载 + 校验
./build/gateway install upgrade.pkg v2                       # 安装 + 切换
./run.sh sim localhost                                       # 守护启动（含回滚判定）
```

**回滚判定必须由外层守护做**，不能由新版本自己做——新版本可能连 main 都进不去。
`run.sh` 的逻辑：`pending` 与 `attempted` 同时存在即判定上次失败，切回旧版本。

---

## 上板实录

| 步骤 | 命令 |
|---|---|
| 串口登录 | MobaXterm 串口会话，CH340，115200 8N1 |
| 网口起链路 | `ip addr show eth0` 确认 `NO-CARRIER` 消失 |
| 取地址 | `udhcpc -i eth0` |
| 虚拟机接入同网段 | VMware 加桥接网卡，桥接到实际物理网卡，`dhclient ens37` |
| 传输 | `ssh root@<板子IP> 'cat > /opt/app/gateway' < build-arm/gateway` |
| 运行 | `chmod +x /opt/app/gateway && /opt/app/gateway sim <brokerIP>` |

踩过的坑：

- 板子 `/root` 被做成了一个 33KB 的**文件**，不是目录，只能换 `/opt/app`
- 板子 SSH 只支持 `ssh-rsa`，需在 `~/.ssh/config` 加
  `HostKeyAlgorithms +ssh-rsa` / `PubkeyAcceptedAlgorithms +ssh-rsa`
- 新版 `scp` 走 SFTP 协议与老服务端不兼容，用 `scp -O` 或 ssh 管道

---

## 设计取舍（面试会问）

**为什么环形缓冲不加锁？**
单生产单消费，`head` 只有采集线程写、`tail` 只有处理线程写，用 `__atomic` 的
release/acquire 保证可见性就够了。采集线程一旦阻塞在锁上，串口数据会直接丢。

**为什么不引 Paho？**
交叉编译第三方库要连带解决依赖链；OTA 场景只需要四个报文，自己写反而可控，
也便于在板子上静态链接。

**OTA 为什么用双目录而不是单目录？**
单目录必须覆盖正在运行的程序，覆盖途中断电就变砖。双目录的代价是多占一份存储，
换来的是任何时刻 `current` 都指向一个完整版本。

---

## 验证情况

| 项目 | 状态 |
|---|---|
| x86 上全链路 | ✅ |
| 交叉编译 + 在 i.MX6ULL 运行 | ✅ |
| 板子 → 局域网 MQTT broker 上报 | ✅ |
| 断网缓存与补发（板上验证过重启补发） | ✅ |
| OTA 下载/校验/切换/回滚（x86 验证） | ✅ |
| 真实串口设备接入 | ⬜ 未接传感器，HAL 接口已就绪 |
