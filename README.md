# ZNVS

从 **Zephyr v4.4.2 NVS** 抽离的独立 C99 键值存储模块。保留其追加写入、
掉电恢复、垃圾回收与重复值跳过机制；不依赖 Zephyr、RTOS、设备树、Kconfig、
POSIX 或动态内存。接口参考 [zat](https://github.com/JiapengLi/zat)：
`znvs_t` 显式实例、调用方持有内存、回调加 `void *arg`、统一状态码。

**接入只需要 `znvs/znvs.c` 和 `znvs/znvs.h`。** 本文件是唯一使用指南；
完整接口约定在头文件。`tests/upstream/` 只用于离线对照测试，不能加入产品编译。

## 接入

复制 `znvs/` 目录，将 `znvs.c` 加入工程，并实现三个同步 Flash 回调：

```c
int flash_read(void *arg, uint32_t off, void *buf, size_t len);
int flash_write(void *arg, uint32_t off, const void *buf, size_t len);
int flash_erase(void *arg, uint32_t off, size_t len);
```

下面是接入片段；三个回调须由平台实现。完整可运行示例见 `examples/basic.c`。

```c
#include "znvs/znvs.h"

static const znvs_cfg_t cfg = {
    .read = flash_read,
    .write = flash_write,
    .erase = flash_erase,
    .size = 8192,        /* 专用分区大小 */
    .sector_size = 4096, /* NVS 逻辑扇区 */
    .erase_size = 4096,  /* 硬件最小擦除单元 */
    .write_size = 4      /* 驱动最小写入粒度，不是 SPI page 大小 */
};
static znvs_t store;

int settings_example(void *flash_context)
{
    uint32_t value = 1234, restored;
    size_t length;
    int rc = znvs_init(&store, &cfg, flash_context);
    if (rc != ZNVS_OK)
        return rc; /* 不要在挂载失败时自动格式化 */
    rc = znvs_write(&store, 1, &value, sizeof(value));
    if (rc != ZNVS_OK)
        return rc;
    rc = znvs_read(&store, 1, &restored, sizeof(restored), &length);
    if (rc != ZNVS_OK)
        return rc;
    return length == sizeof(restored) && restored == value
         ? ZNVS_OK : ZNVS_ECORRUPT;
}
```

示例把本机整数当作字节串保存。需要跨 CPU、编译器或结构体版本迁移的应用，
应自行序列化业务数据；库只规定元数据和 CRC 的小端格式，不转换业务内容。

## Flash 与并发约定

回调偏移均**相对于分区起点**，平台自行加上物理基址。回调必须完成整次操作、
等待硬件结束后才返回 `ZNVS_OK`；任何非零值，包括正数，都映射为 `ZNVS_EIO`。
库提供写入偏移和长度对齐，但源指针可能不对齐。平台负责页边界拆分、源指针对齐、
忙等待及超时、缓存/DMA 一致性和真实物理分区边界保护。读取必须允许任意偏移和长度。

仅面向**擦除值为 `0xff`、编程为 1→0 的 NOR 类介质**，不内置 NAND 坏块/ECC 管理。
几何参数必须真实且固定：逻辑扇区为不超过 64 KiB 的 2 次幂，是擦除单元的整数倍；
擦除单元、写入粒度也为 2 次幂；写入粒度不大于 `ZNVS_IO_SIZE`。
分区包含 2～65535 个完整逻辑扇区，至少保留一个扇区供回收使用。
每扇区还须放得下 4 个对齐后的 8 字节元数据项和至少一个写入块。

`cfg` 在实例使用期间必须保持有效且不可修改，宜放在 `static const` 中。
**同一实例的所有操作，包括读，都由应用串行化**；回调不能重入，不能在中断中调用。
不同实例可管理不同分区，共享 Flash 控制器的互斥由平台完成。全部 API 都是同步阻塞式。

## API 与失败处理

| 操作 | 约定 |
| --- | --- |
| `znvs_init / znvs_mount` | 初始化并恢复 / 重新挂载；全擦除分区也可直接初始化。 |
| `znvs_write / znvs_delete` | 成功返回 0，相同数据不重复编程；写入长度 0 等同删除。 |
| `znvs_read` | 完整读取；长度通过输出参数返回，不静默截断。 |
| `znvs_read_hist` | 第 0 版为最新；删除记录也占一版；旧版本可被 GC 回收。 |
| `znvs_max_size / znvs_available` | O(1) 查询单值上限 / 当前扇区无需 GC 可写的载荷上限；后者不是总空闲量。 |
| `znvs_rotate` | 手动换扇区并 GC，可提前承担擦除延迟；通常不必调用，会增加擦写。 |
| `znvs_format` | **显式清空整个分区并重新挂载，不可撤销，不具备掉电原子性。** |

ID 范围为 `0..65534`，`65535` 保留。不存在可与删除区分的零长度值。
`znvs_read(&store, id, NULL, 0, &length)` 仅查询长度，不校验数据 CRC。
缓冲区过小返回 `ZNVS_ENOSPC` 和所需长度，不修改缓冲区；读取/CRC 失败时不要使用输出。
错误码：`EINVAL=-2`、`ENOSPC=-3`、`EIO=-4`、`ENOENT=-5`、`ESTATE=-6`、`ECORRUPT=-7`。

I/O 失败会令实例失效，后续普通读写返回 `ZNVS_ESTATE`。硬件恢复后调用
`znvs_mount()`，而不是盲目继续写入。失败的写入**可能已经提交**，不能把失败理解为回滚。
挂载可能补写元数据、重做 GC 或擦除被中断 GC 涉及的扇区，但不会因挂载失败自动全盘格式化。
CRC 损坏会报告 `ZNVS_ECORRUPT`，不保证自动找回被破坏的数据。模块没有多键事务、加密或认证。

## RAM 与效率

默认：`ZNVS_DATA_CRC=1`、`ZNVS_CACHE_SIZE=0`、`ZNVS_IO_SIZE=32`。
宏须对库和所有使用该头文件的编译单元保持一致；特别是缓存宏会改变结构体 ABI。

| 配置 | Cortex-M3 实例 RAM | ARM 对象 text 字节 | 写入调用链局部栈估算 |
| --- | ---: | ---: | ---: |
| 默认：CRC=1，cache=0，IO=32 | **20 B** | 4918 | ≤504 B |
| 查找缓存：CRC=1，cache=8，IO=32 | 52 B | 5192 | ≤512 B |
| 小缓冲：CRC=0，cache=0，IO=8 | 20 B | 4576 | ≤416 B |

以上为 Clang 17、Cortex-M3 Thumb、`-Oz -ffreestanding -fno-builtin` 的编译结果。
对象 `data/bss` 均为 0；text 含只读常量及对象元数据，**不是完整固件链接尺寸**。
`znvs_cfg_t` 在该 ABI 下为 28 B，可置于 ROM；x86-64 下默认实例为 32 B、配置为 40 B。
业务缓冲区和驱动上下文由应用另行提供。CRC 表使用只读存储，无全量键索引或整扇区 RAM 镜像。

**20 B 只是常驻实例，不是总 RAM。** 栈列是编译器 `.su` 与本模块汇编直接调用图的保守求和，
包含尾调用叠加，未计 C 库、编译器外部辅助函数、Flash 回调和中断栈，也不是板上测量峰值。
请结合真实驱动调用链及栈水位测量配置线程/主栈，不要把 32 B 的工作缓冲大小当成栈需求。

默认节省 RAM，查找仍需扫描元数据，GC 在最坏情况下会有二次扫描成本。
读操作较多、SPI 单次访问开销较高时可选 `ZNVS_CACHE_SIZE=8`，增加 32 B 缓存项存储；
哈希冲突仍会回退扫描，不承诺恒定 O(1) 查找。缓存关闭或为不超过 32768 的 2 次幂。
`ZNVS_IO_SIZE` 可选 8～256 的 2 次幂；较小缓冲减少局部栈、增加分块 I/O 次数。
本次移植把掉电残留数据定位改为反向线性扫描，避免反复扫描同一大段区域。
对齐整块数据尽量直接送驱动，小块搬移只使用固定缓冲。实际延迟仍取决于硬件擦写及驱动。

CRC32 每个非空值增加 4 B 存储并增加计算量，元数据 CRC8 始终保留。
**已有分区不能直接切换 `ZNVS_DATA_CRC`**；需备份迁移或显式格式化。
默认开启 CRC 是为了读出损坏检测，小缓冲/关闭 CRC 配置应按硬件与可靠性要求选择。

## 构建与验证

```sh
make test              # 默认配置测试；日志在 _build/
make demo              # 运行 RAM 模拟 Flash 示例
make matrix            # 8 组配置 + 原版镜像/交叉挂载对照；需要 Python 3.8+
make sanitize          # Clang ASan + UBSan，默认及 8 槽缓存配置
make measure           # Clang 交叉编译 + GNU size/nm；无需安装 ARM C 库
make test CACHE=8 IO=32 # Make 使用独立配置输出目录，避免复用旧 ABI 对象
```

```sh
cmake -S . -B _build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build _build/cmake
ctest --test-dir _build/cmake --output-on-failure
```

作为子项目：`add_subdirectory(path/to/znvs)`，然后
`target_link_libraries(app PRIVATE znvs::znvs)`。CMake 会传播配置宏；子项目默认不构建主机测试和示例。

交付测试记录见 `tests/RESULTS.txt`。测试覆盖读写、删除、历史、长度边界、满盘、回收、
1～256 字节写入粒度、64 KiB 扇区、CRC 损坏、I/O 失败，以及写入/擦除逐字节截断和部分位变化。
这些是特定 NOR 模型下的故障注入，**不等同于所有掉电方式的数学证明或真实硬件认证**。
测试不能覆盖芯片内部 ECC、擦除扰动、邻近位损坏、驱动超时或板级电源时序；产品上仍需实测掉电。

`tests/upstream/` 保留经 Git blob SHA 校验的原版核心，在主机适配层上做镜像逐字节比较和双向挂载。
在相同几何、CRC 选项及小端格式条件下对照通过；未在真实 Zephyr 板卡或大端 CPU 上运行。
不支持把另一种配置/任意版本的已有分区直接当作兼容数据。

## 来源与许可

基线：[Zephyr v4.4.2 NVS](https://github.com/zephyrproject-rtos/zephyr/tree/v4.4.2/subsys/kvss/nvs)。
原理：[Zephyr NVS 文档](https://docs.zephyrproject.org/latest/services/storage/nvs/nvs.html)。
来源校验、改动摘要见 `NOTICE`；Apache-2.0，保留上游版权声明，完整许可见 `LICENSE`。
