# ZNVS v2

用于 NOR Flash 的独立 C99 键值日志，无 OS、堆、设备树或 POSIX 依赖。接入只需要 `znvs/znvs.c` 和 `znvs/znvs.h`。

v2 将原来的 Zephyr NVS 扇区环改为**两个等大的存储区**。一个区保存当前状态，另一个区构建下一份完整快照；快照最后提交，掉电后选择已提交的新区或旧区。以分区约一半的可用容量，换取较小的代码和简单的恢复流程。

**v2 不兼容 v1 / Zephyr NVS Flash 格式，也改变了实例 ABI。** 升级前用旧程序导出有效数据，再显式格式化或使用新分区，最后通过 v2 API 写回。不能直接覆盖旧固件后原地挂载。识别失败不会自动清空分区。

## 构建版本：只选一次

公共头文件不含功能配置宏。CRC32 始终启用，I/O 缓冲固定为 32 B，不使用常驻查找缓存。三个版本共用相同的头文件、实例布局和 Flash 格式；应用编译单元无需定义任何配置宏。

| 版本 | 用途 | 保留的 API |
| --- | --- | --- |
| `full`，默认 | 应用程序，自动创建空分区、相同值跳过写入 | 全部 |
| `boot` | 约 2 KiB 的读写 bootloader | init、mount、read、write、delete、max_size |
| `readonly` | 只读取既有配置，允许 write/erase 回调为空 | init、mount、read、max_size |

```sh
make                       # 完整版
make PROFILE=boot          # bootloader 读写版
make PROFILE=readonly      # 只读版
```

也可用 CMake 的 `-DZNVS_PROFILE=boot`。手动编译时，**只对 znvs.c** 指定私有构建选择 `-DZNVS_PROFILE=1`（boot）或 `=2`（readonly）；不指定就是完整版。不要向业务代码传播它，也不需要配置头文件。Flash 几何仅在 `znvs_cfg_t` 中填写一次，不再提供编译期重复配置。

`boot` 和 `readonly` 需要已经初始化的 v2 分区，由应用或生产镜像准备。boot 不做相同值比较，非空写入总是追加；应避免重复保存没有变化的数据。三个版本都保留 CRC32 和相同的恢复校验；完整应用与 boot 可以交替更新同一分区。

Clang 22.1.8、Cortex-M3 Thumb、`-Oz -flto` 的实测结果如下。所有版本均支持运行时几何，不依赖固定分区大小或写入粒度。

| 版本 | 未链接对象 text | 链接后的库 ROM | C 辅助函数 | 合计 ROM | 实例 RAM | 模块局部调用链栈估算 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| boot 读写 | 2278 B | 2064 B | 32 B | **2096 B，约 2.05 KiB** | 32 B | 写入 ≤280 B |
| readonly | 1114 B | 960 B | 0 B | **960 B** | 32 B | 读取 ≤136 B |
| full | 3004 B | 2484 B | 60 B | 2544 B | 32 B | 写入 ≤376 B |

boot 合计比严格的 2048 B 多 48 B。链接保留读写版的 init/read/write/delete、只读版的 init/read；额外使用其他 API 会改变尺寸。ROM 包含 ARM 展开表，不含 Flash 驱动、向量表、启动代码、应用配置和业务缓冲。C 辅助函数是尺寸验证用的 memcpy/memset/memcmp，单独编译、不参与 LTO；真实工程共享 C 库时新增成本可能不同。完整 API 全部保留时，以对象尺寸为更保守的参考。详细选项和分段尺寸由 `tools/measure.py` 输出。

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

三个回调为：

```c
int flash_read(void *arg, uint32_t off, void *buf, size_t len);
int flash_write(void *arg, uint32_t off, const void *buf, size_t len);
int flash_erase(void *arg, uint32_t off, size_t len);
```

偏移相对于分区起点。回调必须同步完成整次操作后才返回 0；任意非零值均转换为 `ZNVS_EIO`。读取允许任意偏移、长度；写入地址和长度按 `write_size` 对齐，源指针可能不对齐。驱动负责跨页拆分、等待完成、超时、源指针对齐、缓存/DMA 一致性和物理分区边界保护。

介质必须具有擦除值 `0xff`、编程 1→0 的 NOR 语义。失败可留下本次操作范围内任意子集的编程/擦除位，而不只支持逐字节前缀截断。回调返回成功必须表示操作已真实完成；邻近单元扰动、隐藏 ECC 状态、坏块、控制器错误恢复仍由平台处理。所有操作都由应用串行化，包括读；回调不得重入实例，也不是 ISR API。

`cfg` 必须始终有效且不可修改，宜使用 `static const`。只需设置总容量 `size`、物理擦除单元 `erase_size` 和编程粒度 `write_size`。擦除单元和编程粒度均为 2 的幂，编程粒度支持 1、2、4、8、16、32 B；两个区必须等大且都按这两个粒度对齐，能够容纳区头和记录。已删除无实际作用的逻辑 `sector_size`。

业务数据按原始字节保存。示例保存本机整数，仅适用于相同序列化约定；跨 CPU 或结构体版本升级，应由应用显式序列化。

## 提交与恢复

每条记录包含 16 B 元数据：8 B 的 ID、长度和数据地址，以及逐位取反的副本。元数据与反码之间任意未完成的 1→0 编程，都不能伪装成另一条完整合法记录。提交标记位于**另一个完整编程单元**。

1. 先写元数据，预留完整载荷范围。
2. 再写载荷及 CRC32；CRC 覆盖 ID、长度和载荷，独立存放于对齐后的尾部。
3. 最后写提交单元的首字节；任意 0 位出现即表示提交。此时之前的数据操作已全部成功。

因此，回调报错时记录仍可能已经提交，不能假定回滚。元数据写入失败不会继续写提交标记；数据失败留下的完整预留仍会占用空间，挂载后不会覆盖这些未擦除单元。GC 会回收无效预留。

读和挂载遇到**无效但已提交**的元数据时返回 `ECORRUPT`，不跳过它去返回旧版本或使删除的数据重新出现。读出载荷 CRC 错误也返回 `ECORRUPT`，但允许应用用已知正确的数据重新写入。GC 校验每条被保留的数据，发现坏 CRC 就放弃目标区，不提交它，也不擦除源区；不能把历史读取当作长期备份。

GC 先计算保留数据的空间需求。放不下时直接返回 `ENOSPC`，**不写、不擦**。能放下才擦除另一区、复制有效记录、写入待更新值，最后提交新区头。区头采用两份独立互补编码，任意一份完整即可提交；两份均成功写入后，单个位翻转不会使系统选择旧快照。旧区保留到下一次 GC 才擦除。

已有有效区时，挂载只读，不擦除、不补写。完整构建只会自动初始化全空分区，或在整个剩余分区仍为空时恢复首次区头写入；重试残缺首次区头前先擦除该空区。未知格式、无法解释的介质和已提交元数据损坏不会触发自动格式化。

互补编码保护的是所述单次 NOR 操作的部分完成。任意多位保留错误可能同时破坏编码、提交标志或两份区头；CRC 也不是纠错、认证或全故障证明。关键数据的业务备份与硬件掉电测试仍不可省略。

## API 与错误

| API | 约定 |
| --- | --- |
| `znvs_init / znvs_mount` | 配置并挂载 / I/O 恢复后重新挂载。 |
| `znvs_write / znvs_delete` | 成功为 0；长度 0 等于删除，没有独立空值。 |
| `znvs_read` | 完整读取，不静默截断。 |
| `znvs_read_hist` | 0 为最新；删除占一个版本；GC 会丢弃历史，仅完整构建提供。 |
| `znvs_max_size` | 单值最大载荷，不代表当前可用空间。 |
| `znvs_available` | 当前区无需 GC 可写的载荷上限，仅完整构建提供。 |
| `znvs_rotate` | 显式构建下一快照，会擦写，仅完整读写构建提供。 |
| `znvs_format` | 显式清空整个分区，不具备掉电原子性；中断后由调用方明确重试。 |

ID 为 `0..65534`，65535 保留。查询长度使用 `data=NULL, capacity=0, length!=NULL`，不验证载荷 CRC。缓冲区不足返回 `ENOSPC` 和所需长度，不修改输出缓冲；读取/CRC 失败时不得使用输出。

错误码为 `EINVAL=-2`、`ENOSPC=-3`、`EIO=-4`、`ENOENT=-5`、`ESTATE=-6`、`ECORRUPT=-7`。I/O 失败和无效已提交元数据会使实例失效；修复设备后重新挂载。单次读取的载荷 CRC 错误不使实例失效，便于应用重写正确值。重新挂载不会修复永久损坏，也不保证成功。

没有多键事务。需要原子更新的多个字段应组成同一记录；应用锁也应覆盖“查询长度→分配缓冲→读取”等需要一致性的组合操作。

## 容量、RAM 与效率

令 `W` 为写入粒度，`B` 为半个分区，`align(n)` 为向上对齐到 W：

- 区头占 `align(32)`。
- 每个记录槽占 `align(16) + W`。
- 非空载荷占 `align(len) + align(4)`；删除只占记录槽。
- 最大载荷为 `min(65535, 向下对齐(B - align(32) - 记录槽 - CRC单元))`。

例如 2×4096 B、W=4：单条最大 4040 B；4 B 数值占 28 B。独立提交和互补副本比 v1 增加了每条记录的空间及编程次数，这是可靠性的代价。GC 搬移整个区的有效快照，分区很大或冷数据很多时，应评估写放大。

完整版 GC 使用固定 32 B 临时位表，按最新到最旧遍历；未命中过的 ID 无需再查找，发生碰撞才执行精确查找。它不承担正确性判断，不会因哈希碰撞漏拷贝。boot 省略这张临时表以节省代码。两者都没有常驻查找缓存。

在相同的两区 4 KiB、128 个不同 ID、每值 4 B 的测试中：

| 操作 | v1 默认 | v2 完整默认 |
| --- | ---: | ---: |
| GC 读回调 | 17287 | **640** |
| GC 写回调 | 258 | 513 |
| 空区重新挂载读回调 | 771 | **3** |
| 满盘后失败写入的擦除次数 | 2 | **0** |

这是回调次数，不是芯片耗时。独立提交增加了写回调，实际总延迟取决于读访问成本、编程时间和擦除时间。普通查找仍为 O(N)，GC 过滤表碰撞或关闭后最坏仍可能二次扫描；此库主要面向少量配置，不承诺硬实时延迟。

搬移缓冲固定为 32 B。CRC32 使用逐位实现，节省常量表 ROM，但比查表更耗 CPU。

32 位 ABI 的实例为 32 B、配置为 24 B，配置可置于 ROM；x86-64 分别为 40 B、40 B。实例内缓存几个几何计算结果，用 4 B 额外 RAM 换取更少的重复指针读取和对齐运算。没有动态分配和整区 RAM 镜像。表中栈为编译器帧和本模块调用链的保守估算，不含驱动、C 库、编译器外部辅助函数和中断，也不是板上实测的完整线程栈。

## 构建和验证

```sh
make test
make demo
make matrix
make sanitize
make measure
make PROFILE=boot
```

Make 按版本使用独立输出目录。CMake 的功能版本选择仅作用于库本身：

```sh
cmake -S . -B _build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build _build/cmake
ctest --test-dir _build/cmake --output-on-failure

cmake -S . -B _build/boot -DZNVS_PROFILE=boot
```

尺寸表的 LTO 需要在最终固件链接中启用；仅把库编译为静态库不会自动获得表中链接尺寸。Clang 示例使用 `-Oz -flto`，并由固件链接启用 `--gc-sections`。其他编译器/MCU 必须重测。

离线工具可显式指定编译器：

```sh
python tools/check.py --cc gcc
python tools/check.py --cc clang --sanitize-only
python tools/measure.py --clang clang --size llvm-size --nm llvm-nm
```

测试包括全部六种编程粒度、随机模型、空间耗尽零擦写、逐字节及全操作非顺序部分编程/擦除、首次初始化重复掉电、恢复读错误、元数据逐位损坏、删除不复活、坏 CRC 不被 GC 发布，以及完整/bootloader/只读构建互操作。`tests/bench_znvs.c` 记录 I/O 次数。上游测试现在比较逻辑值，并验证 v2 拒绝且不修改 v1 镜像，**不再声称镜像兼容**。

代码风格由 `.clang-format` 固定：4 空格缩进、控制块保留花括号、`ColumnLimit: 0`，不因长度而拆分函数签名或表达式。修改保留文件现有换行风格，新文件使用 LF。

验证记录见 `tests/RESULTS.txt`。来源和许可见 `NOTICE`、`LICENSE`；历史上游源码保留在 `tests/upstream/`，不参与产品编译。
