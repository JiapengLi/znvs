# ZNVS

用于 NOR Flash 的独立 C99 键值存储，无 OS、堆或 POSIX 依赖。接入只需要 `znvs/znvs.c` 和 `znvs/znvs.h`。主要面向不超过 8 个键的配置存储，同时验证 32、64 个键；键数没有额外的 API 上限。

分区分为两个等大的区，一个保存当前状态，另一个构建下一份快照。以约一半分区容量换取简单的提交和掉电恢复流程。

## 构建

公共头文件没有功能配置宏。CRC32 固定启用，I/O 缓冲固定 32 B，无常驻查找缓存。三个构建共用头文件、实例布局和介质格式。

| 构建 | 用途与 API |
| --- | --- |
| `full`，默认 | 全部 API；自动创建空分区，相同值跳过写入。 |
| `boot` | init、mount、read、write、delete、max_size；非空写入总是追加。 |
| `readonly` | init、mount、read、max_size；write/erase 回调可为空。 |

```sh
make
make PROFILE=boot
make PROFILE=readonly
```

CMake 使用 `-DZNVS_PROFILE=boot` 或 `readonly`。手动编译时，仅对 `znvs.c` 指定私有选项 `-DZNVS_PROFILE=1`（boot）或 `=2`（readonly），默认完整构建；应用无需同步定义。boot 和 readonly 需要已初始化的分区，由完整构建或生产镜像准备。

## 接入

```c
#include "znvs/znvs.h"

static const znvs_cfg_t cfg = {
    .read = flash_read,
    .write = flash_write,
    .erase = flash_erase,
    .size = 8192,
    .erase_size = 4096,
    .write_size = 4
};
static znvs_t store;

int save_flags(void *flash_context, uint32_t flags)
{
    int rc = znvs_init(&store, &cfg, flash_context);
    if (rc != ZNVS_OK) {
        return rc;
    }
    return znvs_write(&store, 1, &flags, sizeof(flags));
}
```

回调签名如下，偏移相对于分区起点：

```c
int flash_read(void *arg, uint32_t off, void *buf, size_t len);
int flash_write(void *arg, uint32_t off, const void *buf, size_t len);
int flash_erase(void *arg, uint32_t off, size_t len);
```

回调必须同步完成整次操作后返回 0，任意非零值转换为 `ZNVS_EIO`。读取允许任意偏移、长度；写入地址和长度按 `write_size` 对齐，源指针可能不对齐。驱动负责跨页拆分、等待完成、超时、源指针对齐、缓存/DMA 一致性及物理分区边界保护。

介质须为擦除值 `0xff`、编程只允许 1→0 的 NOR。擦除单元为 2 的幂；编程粒度支持 1、2、4、8、16、32 B。两个区必须等大、按擦除和编程粒度对齐，并能容纳区头与记录。`cfg` 须始终有效且不可修改，宜使用 `static const`。

同一分区只能有一个活动写实例。包括读取在内的所有操作都须由应用串行化，回调不得重入，也不是 ISR API。其他程序或实例修改分区后，旧实例必须重新挂载；应用与 bootloader 交接时也适用。不同分区共用 Flash 硬件的访问由驱动协调。

业务数据按原始字节保存。跨 CPU 或结构体版本使用时，由应用显式序列化。没有多键事务，需要原子更新的字段应组成同一记录。

## API 与错误

| API | 约定 |
| --- | --- |
| `znvs_init / znvs_mount` | 配置并挂载 / 恢复 I/O 或交接分区后重新挂载。 |
| `znvs_write / znvs_delete` | 成功返回 0；长度 0 等于删除，没有独立空值。 |
| `znvs_read` | 完整读取，不静默截断。 |
| `znvs_read_hist` | 历史版本读取；删除占一个版本，GC 会丢弃历史。仅 full。 |
| `znvs_max_size / znvs_available` | 单值载荷上限 / 当前区无需 GC 可写的载荷上限。后者仅 full。 |
| `znvs_rotate` | 主动 GC，会擦写；可用于移开时延敏感路径。仅 full。 |
| `znvs_format` | 清空分区，不具备掉电原子性；中断后由调用方明确重试。仅 full。 |

ID 为 `0..65534`。`znvs_read(data=NULL, capacity=0, length!=NULL)` 查询长度，不检查载荷 CRC；缓冲不足返回 `ENOSPC` 和所需长度，缓冲不变。读取/CRC 失败后不得使用输出。需要一致性的“查长度→分配→读取”组合也应整体加锁。

错误码：`EINVAL=-2`、`ENOSPC=-3`、`EIO=-4`、`ENOENT=-5`、`ESTATE=-6`、`ECORRUPT=-7`。I/O 失败或已提交元数据损坏使实例失效，恢复设备后须重新挂载。失败的写入可能已提交，不能假定回滚。载荷 CRC 错误不使实例失效，允许用已知正确值重写；重新挂载不修复永久损坏。

## 提交与恢复

每条记录先写 20 B 元数据（ID、长度、地址、反码和独立 CRC32），再写载荷及绑定 ID/长度的 CRC32，最后写独立提交单元。已提交元数据在参与查找前校验，包括删除记录；检测到损坏即报错，不跳过坏记录返回旧值。未提交记录保守保留数据范围，避免重用未擦除单元，由 GC 回收。

GC 先检查容量，`ENOSPC` 不写不擦；能容纳才擦除备用区、校验并复制有效值、写待更新值，最后发布两份互补编码的区头，其签名绑定分区大小和编程粒度。旧区保留到下一次 GC。已有有效区时挂载只读；完整构建仅对全空介质或可识别的首次区头写入中断自动初始化，不因普通挂载错误清空分区。

恢复支持单次编程/擦除范围内任意子集的部分完成。两份区头均写完后，单比特损坏不会导致旧快照回退。两份区头同时损坏可能与未完成发布无法区分，仍可能回退旧快照；CRC 不提供纠错、认证或任意损坏下的防回滚保证。真实器件的 ECC、邻近扰动和物理断电行为需上板验证。

## 容量、RAM 与 ROM

令 `W` 为编程粒度、`B` 为半分区、`align(n)` 为向上对齐到 W：区头占 32 B，记录槽占 `align(20)+W`，非空载荷占 `align(len)+align(4)`，删除只占记录槽。2×4096 B、W=4 时，最大单值 4036 B，4 B 值占 32 B。

以下为 Cortex-M3 Thumb、Clang 22.1.8、`-Oz -flto` 测量，几何在运行时配置：

| 构建 | 链接 ROM，含 C 辅助函数 | 实例 RAM | 模块局部调用链栈估算 |
| --- | ---: | ---: | ---: |
| boot | **2096 B，约 2.05 KiB** | 32 B | 写入 ≤280 B |
| readonly | **982 B** | 32 B | 读取 ≤136 B |
| full | **2544 B** | 32 B | 写入 ≤376 B |

boot 比严格 2 KiB 多 48 B。测量保留 init/read，以及读写构建的 write/delete；使用其他 API 会改变尺寸。ROM 含 ARM 展开表，不含驱动、启动代码和应用配置；最终固件须启用 LTO，其他 MCU/编译器需重测。32 位 ABI 的配置为 24 B，可置于 ROM；x86-64 实例和配置均为 40 B。栈估算不含驱动、C 库与中断。没有堆分配或整区 RAM 镜像。

## 小键数性能

以下为 full 构建、2×4096 B 分区、W=4、每值 4 B 的实测回调数。full GC 使用 32 B 临时过滤位表，碰撞时精确查找；boot 省略该表，不能直接套用 GC 数值。

| 操作 | 8 键 | 32 键 | 64 键 |
| --- | ---: | ---: | ---: |
| 有数据重新挂载：读回调 | 11 | 35 | 67 |
| 读取最早的键：读回调 / 字节 | 10 / 176 B | 34 / 680 B | 66 / 1352 B |
| GC：读回调 | 160 | 256 | 384 |
| GC：写回调 / 擦回调 | 33 / 1 | 129 / 1 | 257 / 1 |

这些不是硬件耗时。普通查找为 O(N)，GC 最坏可能二次扫描；逐位 CRC32 节省 ROM，但消耗 CPU。更新 4 B 值、不触发 GC 时为 4 次写回调；删除为 2 次。少量键无需常驻查找表，真实延迟仍取决于驱动与器件，不承诺硬实时。

## 验证

```sh
make test
make demo
make matrix
make sanitize
make measure
cmake -S . -B _build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build _build/cmake
ctest --test-dir _build/cmake --output-on-failure
```

离线工具可指定编译器：`python tools/check.py --cc gcc`、`python tools/check.py --cc clang --sanitize-only`、`python tools/measure.py --clang clang --size llvm-size --nm llvm-nm`。测试覆盖六种编程粒度、随机模型、写擦中断、连续恢复故障、元数据损坏、GC 校验及三种构建互操作。详情见 `tests/RESULTS.txt`。

`.clang-format` 使用 4 空格、控制块花括号、`ColumnLimit: 0`。来源和许可见 `NOTICE`、`LICENSE`；`tests/upstream/` 仅用于测试，不参与产品编译。
