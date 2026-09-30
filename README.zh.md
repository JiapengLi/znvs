# ZNVS

[English](README.md)

用于 NOR Flash 的小型独立 C99 键值存储，无 OS、堆或 POSIX 依赖。接入只需 [znvs.c](znvs/znvs.c) 和 [znvs.h](znvs/znvs.h)。

面向通常不超过 8 个键的配置存储，同时验证 32、64 个键。分区分为两个等大的区：写入追加到当前区，垃圾回收（GC）将有效值复制到另一区，再发布新状态。

- 元数据与载荷均有 CRC32，记录最后写入独立提交单元。
- 写入中断保留已提交前缀；未完成的尾记录封存当前区，下次实际修改先执行 GC。
- 两区交替擦除，每次 GC 复制全部有效值；没有冷热分离或坏块替换。
- 无常驻查找缓存；查找扫描记录，GC 最坏为 O(N²)，N 为累积记录数。所有操作同步执行。

## 快速接入

实现以下回调，`addr` 为相对分区起点的字节地址，然后初始化一次：

```c
#include "znvs/znvs.h"

int znvs_port_read(void *arg, uint32_t addr, void *buf, size_t len);
int znvs_port_write(void *arg, uint32_t addr, const void *buf, size_t len);
int znvs_port_erase(void *arg, uint32_t addr, size_t len);

static znvs_t store;

int znvs_store_init(void *flash_context)
{
    const znvs_cfg_t cfg = {
        .read = znvs_port_read,
        .write = znvs_port_write,
        .erase = znvs_port_erase,
        .size = 8192,
        .erase_size = 4096,
        .write_size = 4
    };
    return znvs_init(&store, &cfg, flash_context);
}

int znvs_save_flags(uint32_t flags)
{
    return znvs_write(&store, 1, &flags, sizeof(flags));
}
```

`znvs_store_init` 成功后再调用 `znvs_save_flags` 或 `znvs_read`。实例完整保存配置副本，局部 `cfg` 可以离开作用域；作为 `arg` 传入的驱动上下文仍须保持有效。

ID 为 `0..65534`，长度为 0 的写入等同删除。读取返回完整值，不静默截断；仅查询长度（`data=NULL, capacity=0, length!=NULL`）不检查载荷 CRC。完整 API 见[头文件](znvs/znvs.h)，可运行的端口示例见 [examples/basic.c](examples/basic.c)。

## 驱动约定

NOR 擦除值须为 `0xff`，编程只允许 1→0。擦除单元为统一的 2 的幂；编程粒度支持 1、2、4、8、16、32 B。每个区须包含完整擦除/编程单元，并能容纳区头和记录。

回调必须同步完成整次操作后返回 0，非零值转换为 `ZNVS_EIO`。读取允许任意地址和长度；写入地址与长度对齐，但源指针可能不对齐。驱动负责跨页拆分、等待完成、超时、缓存/DMA 一致性及物理分区边界保护。

包括读取在内的所有操作均须串行化，同一分区只允许一个活动写实例；回调不得重入，不用于 ISR。其他实例或 bootloader 修改分区后须重新挂载。数据按原始字节保存，跨平台使用时由应用显式序列化；需要原子更新的字段应放在同一记录中，不提供多键事务。

## 构建

三个构建共用头文件、实例布局与介质格式。CRC32 固定启用，应用无需功能配置宏。

| 构建 | 包含的功能 |
| --- | --- |
| `full`，默认 | 全部 API；自动初始化空分区，跳过相同值写入。 |
| `boot` | init、mount、read、write、delete、max_size；非空写入总是追加。 |
| `readonly` | init、mount、read、max_size；write/erase 回调可为空。 |

使用 `make`、`make PROFILE=boot` 或 `make PROFILE=readonly`。CMake 使用 `-DZNVS_PROFILE=boot` 或 `readonly`。手动编译时，**仅对 znvs.c** 定义 `ZNVS_PROFILE=1`（boot）或 `=2`（readonly）。boot 和 readonly 需要由 full 或生产镜像预先初始化的分区。

## 失败与恢复

- `ZNVS_ENOSPC`：容量不足，写入操作不编程、不擦除。
- `ZNVS_EIO` 或已提交元数据损坏会使实例失效。恢复设备后调用 `znvs_mount`；失败的写入**可能已经提交**，不能假定回滚。
- `ZNVS_ECORRUPT`：CRC 或元数据校验失败。载荷 CRC 错误允许用已知正确值重写；重新挂载不能修复永久损坏。任何读取错误后均不得使用输出。
- GC 校验复制的载荷后再发布新状态，旧区保留到下次 GC。`znvs_format` 清空分区，**不具备掉电原子性**，中断后须明确重试。

恢复测试覆盖编程/擦除操作部分完成。CRC 不提供纠错或认证；发布区头失效时仍可能选中旧快照，CRC 不保证任意损坏下的防回滚。实际掉电行为需要在目标器件上验证。

## 占用与验证

Cortex-M3 Thumb、Clang 22.1.8、`-Oz -flto`，运行时配置几何；保留 init/read，读写构建另保留 write/delete：

| 构建 | 链接 ROM | 实例 RAM | 模块局部调用链栈估算 |
| --- | ---: | ---: | ---: |
| full | 2622 B | 52 B | 写入 ≤352 B |
| boot | 2136 B | 52 B | 写入 ≤272 B |
| readonly | 972 B | 52 B | 读取 ≤144 B |

ROM 含 C 辅助函数和 ARM 展开表，不含驱动、启动代码及应用。RAM 已包含配置副本；栈估算不含驱动、C 库及中断。其他目标或保留 API 需重新测量。2×4096 B、4 B 编程时，4 B 值占 20 B，最大单值载荷为 4048 B。

运行 `make test`、`make matrix`、`make sanitize`、`make measure`。故障注入、随机模型、构建互操作、性能计数与测量条件见 [tests/RESULTS.txt](tests/RESULTS.txt)。主机测试不能替代硬件验证。

Apache-2.0 许可，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。`tests/upstream/` 中的 Zephyr 参考代码仅参与测试。
