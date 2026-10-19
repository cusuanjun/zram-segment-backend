# 第六阶段：显式 segment GC 与空闲段预留

## 实现范围
新增 zram_backend_gc_segment(zram, segment_id, segment_generation)。
指定源段，保持压缩/RAW 字节原样，搬到同 owner 的全新空段；保留记录 ID。
这是同步内部 API，没有自动选段、后台线程、sysfs 入口或 zRAM slot 接入。
没有修改 F2FS、分区、swap 设置或真实设备。

## 为什么第一版只用全新空段
源段有效字节总和不超过段容量。GC 将有效记录连续拼接，仅最后一次 I/O
补齐，因此 round_up(live_bytes, block_size) 不超过一个段的容量。
记录可以跨写缓冲区边界，不能因到达记录数或缓冲边界而给每条记录加 padding。
这样可用一个空段容纳整个源段，避免按记录分批补齐导致目标空间膨胀。
不使用已有段尾；代价是不合并不同源段，无法因此减少同 owner 的非空段数。
回收效果是把旧段空洞变成目标段可追加尾部；空闲段总数通常不增加。
当仅剩一个预留段时，新 App 仍可能 ENOSPC；同 App 可继续使用整理后的段尾。
后续需要跨段合并/自动选段策略，才能进一步减少非空段数量。

## 预留规则
- setup 至少需要两个段。
- 普通写入优先使用同 owner 现有 OPEN 尾部。
- 必须分配空段时，仅在可用空段数大于 1 时分配。
- generation 已到 U64_MAX 的 FREE 段不可再次分配，不算可用预留。
- GC 可以使用最后一个可用空段；成功后源段回到空闲池。
- 失败后目标段没有有效映射，回收它，源段不变。
这是逻辑容量预留，不是常驻内存预留，也不保证介质故障、generation 耗尽
或内存不足时有进展。free_segments 统计仍包含 generation 耗尽的 FREE 段。

## 读写与提交
整个操作持有 backend mutex，并要求调用方全过程持有 init_lock 读锁。
1. 检查段编号、generation、状态；过期选择返回 ESTALE。
2. 若整理不能回收至少一个逻辑块的尾部空间，返回 EALREADY，不发 I/O。
3. 找一个可用空段；不存在返回 ENOSPC。
4. 分配两个普通页的读缓冲；失败返回 ENOMEM，源和目标都不改变。
5. 目标继承完整 owner，增加 generation。按段内链表读取有效记录。
6. 每条记录读取覆盖它的对齐范围，从中提取有效字节装入共享写缓冲。
   缓冲满则提交；一条记录允许跨连续的两次 I/O，但仍位于同一 segment。
7. 最后不足块大小的部分清零并提交。
8. 全部 I/O 成功后，transfer_record 更新位置并转移段内链表节点。
   不修改全局链表、记录 ID、owner、格式和长度，不分配新记录元数据。
9. 更新目标 padding，回收已空的源段。

内存提交过程没有分配或 I/O，受同一个 mutex 保护，对其他 API 不暴露半完成状态。
这里的统一提交只针对本次运行中的元数据；不是掉电原子性，也没有持久化恢复。

## 失败处理
在任何读取/写入出错时，全部源记录仍指向旧位置，即使之前几个目标 I/O 成功。
等待同步 I/O 结束后回收目标空段；目标已写字节不可达，下次可以覆盖。
不把这些无引用字节永久记为 failed_bytes，也不发 discard 或承诺安全擦除。
目标 generation 一旦被分配就不回退，重试会使用新的代数。

## 内存与时延边界
复用配置时分配的共享写缓冲，额外只需两个 scratch 页及页指针数组，
不分配整个 segment 大小的缓冲，不复制全段元数据。
读取每条记录仍可能产生重复对齐读取；BIO 仍可能分配失败。
同步全段搬迁期间其他后端 API 会等待锁，时延随有效记录量增加。
本阶段优先建立正确的生命周期，不声称已适合生产回收热路径。

## 验证
新增三个 KUnit 用例：GC 保留 ID/owner/RAW 内容与预留段；读写失败后旧映射
与统计不变、重试及 padding 清零；跨页压缩记录搬迁。
测试设备从两段增为三段，普通写入仍可使用两段，另留一段给 GC。
原有容量与 App 隔离测试继续检查 ENOSPC。

主机测试 tools/testing/zram_backend_gc_test.py 将实际后端源码（排除真实
block_io/configure）与全部 18 个 KUnit 用例函数体放入用户态替身环境编译。
另测缩小 I/O 窗口后的跨提交记录、首次读取分配失败、第二次写失败、后续读失败、
成功重试、600 步写入/释放/GC 混合生命周期及 generation 耗尽/单段配置拒绝。
以 4/16/64 KiB PAGE_SIZE 和 4 KiB 逻辑块执行 GCC/UBSan 检查。
主机替身不验证真实内核锁、分配器、链表实现、BIO、设备或实际 KUnit 框架。
完整目标内核编译、内核 KUnit 和设备验证仍未完成。

回归命令：
```
python3 tools/testing/zram_backend_gc_test.py
python3 tools/testing/zram_backend_index_test.py
python3 tools/testing/zram_backend_plan_test.py
git diff --check
```
