# Linux 智能物联网网关

面向弱网场景的边缘网关：向下通过串口接入设备数据，向上通过 MQTT 对接云端，
支持断网缓存、远程控制与 OTA 升级。目标平台 i.MX6ULL（Cortex-A7，512MB）。

## 架构

```
hal_sim / hal_uart      硬件层，唯一与具体设备绑定的地方
        |
     ringbuf            单生产/单消费无锁环形缓冲，采集与处理解耦
        |
      frame             帧解析：粘包、半包、校验失败重同步
        |
      main              线程编排
```

换硬件只改 `src/hal_uart.c`，其余代码不动。

## 目录

```
include/   hal.h  ringbuf.h  frame.h
src/       main.c  hal_sim.c  hal_uart.c  ringbuf.c  frame.c
tests/     test_ringbuf.c
cmake/     toolchain-arm.cmake
```

## 编译运行（x86 上开发）

```bash
cmake -B build
cmake --build build -j4
./build/gateway          # 用模拟数据源，每 200ms 出一帧
./build/test_ringbuf     # 单元测试，无输出即全部通过
```

## 交叉编译到板子

```bash
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm.cmake
cmake --build build-arm -j4
file build-arm/gateway   # 应为 ELF 32-bit ARM
```

上板运行：

```bash
scp build-arm/gateway root@<板子IP>:/opt/app/
ssh root@<板子IP> '/opt/app/gateway uart'
```

## 五阶段路线

| 阶段 | 内容 | 验收标准 |
|---|---|---|
| 1 | 骨架跑通 | x86 上每秒打印数帧 |
| 2 | MQTT 上报 | mosquitto 订阅端能看到消息 |
| 3 | 断网缓存 | 拔网线 10 分钟，数据不丢 |
| 4 | OTA 升级 | 拉包、校验、重启生效，失败可回滚 |
| 5 | 上板集成 | 板子上跑通整条链路 |

## 面试要能讲的三点

1. **为什么用环形缓冲而不是队列**：采集线程不能阻塞在锁上，否则串口数据丢失；
   单生产单消费场景下 `head`/`tail` 两个原子变量足够，无需加锁。
2. **粘包怎么处理**：状态机逐字节找帧头 `AA 55`，凑满 12 字节校验，
   失败时只丢一个字节重新同步，不丢整段。
3. **为什么抽象 hal**：硬件相关代码集中在两个文件里，业务逻辑可在 x86 上
   开发和单元测试，上板只需替换实现。
