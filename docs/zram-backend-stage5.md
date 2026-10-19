# 第五阶段：segment 内有效记录索引（GC 的前置工作）

状态：索引维护已实现，尚未实现 GC 数据搬迁、自动触发或预留段策略。
没有启用真实 App provider、生产下沉或 F2FS/UFS 设备接入。

## 数据结构与成本
每个 segment 增加 record_list 链表头；每个 record 增加 segment_node。
原 node 继续服务全局记录链表和 ID 查找。两个链表节点指向同一个 record，
不会复制压缩数据，也不会为链表节点单独分配对象。
典型 64 位平台每个段头和每条记录各增加 16 字节结构空间；
实际 slab 分配类别与对齐仍需在目标配置测量。
全局 ID 查找仍是线性扫描，本次没有改为 XArray。

## 生命周期
- setup_segments：配置成功时初始化所有段的空链表，不创建任何有效记录。
- write_one：I/O 成功、记录位置已填写后，通过 publish_record 同时加入全局表
  和段表，逐记录更新 records/live_bytes，最后才返回对应记录 ID。
- I/O 或记录准备失败：未发布记录不挂入任何表；按原规则处理失败空间。
- release：unpublish_record 同时摘除两个节点，减少 records/live_bytes，
  增加 invalid_bytes，之后释放对象，再尝试回收空段。
- recycle_empty_segment：记录数为零后还检查链表为空、live_bytes 为零，
  不一致则告警并拒绝回收；不通过重置链表头掩盖遗留节点。
- destroy：在既有调用方已排空请求的前提下，逐条摘除两个索引并释放记录，
  再释放段表。销毁本身不会替调用方排空在途任务。

所有生产索引更新仍受 backend->lock 保护。没有向外提供裸记录指针。
段内链表当前按发布顺序排列，不能把这种顺序当成未来 GC 必须保持的磁盘顺序。

## 不变量
在受保护的稳定状态：
1. 每条已发布记录恰好在全局索引和其所属段索引中各出现一次。
2. 段内记录的 owner、segment_id、segment_generation 与所在段一致。
3. 链表长度 == segment.records。
4. 段内 payload_len 总和 == segment.live_bytes。
5. append_tail == live + invalid + padding + failed。
6. FREE 段没有有效记录及已使用空间。

GC 将来需要专用迁移提交操作：保持全局 record ID 不变，写成功后改变位置、
转移 segment_node 并调整源/目标统计。不能直接把 release + 新 write 当作迁移。

## 测试与验证
新增三个 KUnit 用例：
- 删除中间记录、重复释放、删除头尾、空段换 owner 复用。
- 有旧记录时写失败、另一 App 新段写失败，均不产生索引条目。
- 同一 owner 写入多个段，检查各段 ID 集合，再逐条释放。
另在每个后端 KUnit 用例结束时执行一致性审计，覆盖此前的分组、部分失败等场景。
测试辅助函数仅在 KUnit 配置下构建，返回 ID 快照或审计结果，不是生产 GC API。
KUnit 和完整内核尚未编译/执行。

主机测试 tools/testing/zram_backend_index_test.py 直接提取生产结构及
publish_record/unpublish_record/recycle_empty_segment，配合用户态链表替身，
在 GCC/UBSan 下通过删除/复用/回收保护示例及 20,000 次混合操作。
验证的是元数据辅助函数，不是内核链表实现、锁、BIO、分配失败或生产调用链。
原打包函数主机测试亦作为回归检查；不能以此宣称实际内核 GC 已可用。
