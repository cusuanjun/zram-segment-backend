# 第三阶段：应用归属登记与 slot 传递

状态：内部机制已实现；目标设备归属适配器、用户态登记接口尚未实现。
**未自动识别真实 App，未启用压缩下沉；尚未编译及运行 KUnit。**

## 身份语义
- 按完整 Android UID 对应的应用组分类，不声称按包名严格隔离。
- owner.id = (u64)uid + 1，0 保留为未知；UID 0 有效，UID UINT_MAX 拒绝。
- generation 是应用身份生命周期代次，非 PID、进程启动次数或 segment 代次。
- 同一应用身份的多个 memcg 登记相同 UID/generation 即合并。
- 隔离进程、共享 UID、共享页的特殊归属仍需平台策略；普通共享页采用记账归属。
- memcg_cookie 必须由适配器提供，跨 memcg 生命周期不可复用。不能传裸指针、
  可复用的短 memcg ID、blkcg ID 或 current_uid()。
- 每设备有独立映射表；目标适配器负责把映射登记到需要处理该应用的设备。
- 应用 generation 的分配和不复用由可信登记方负责，不由此哈希表自动推断。

## 已实现路径
zram_owner_bind(cookie, uid, generation)
  -> memcg_cookie 到 owner 的哈希表
目标 provider(page)
  -> 原始页记账归属的 memcg_cookie
zcomp_compress()
  -> zram_owner_capture() 快照
  -> SAME 直接提交，或同步/异步 cookie 携带 owner
zram_slot_update()
  -> 数据及 owner 在同一槽锁内安装
zram_free_page()
  -> 清理 owner，包括空槽、SAME、RAW、旧 WB 等路径

普通写失败不调用成功的 slot_update，不提前改写旧 owner。
普通已分配临时页、非匿名页、非 swap-cache 页、复合页不进行身份识别。
部分写使用临时页，保守归为未知；不推测部分内容归属。
owner 是数值快照，不持有 task/memcg 指针；注销映射不清除已有 slot 或请求快照。
旧 writeback 仍按原路径处理，其释放压缩对象时会清掉 owner；本阶段不承诺旧
后端页保留 App 元数据，也不支持旧后端在线迁移到新后端。

## 接口与生命周期
- zram_owner_init/destroy 随 zRAM 元数据创建/释放；销毁前排空设备 I/O、
  压缩 cookie 和所有控制接口调用。
- bind/unbind 是内部可信 API；不是可从 shell 使用的 sysfs/ioctl。
- 同 cookie 同身份重复登记成功，不同身份返回 EEXIST，需先注销。
  注销后也不能把旧 cookie 给新的 memcg 实例使用。
- provider 在读自旋锁内执行，不得睡眠、分配或重入归属接口。
  必须自行保证读取页记账身份的合法性；模块代码和 private 在注销完成前有效。
- provider 注销取得写锁，等现有回调结束；调用者串行化 provider 控制操作。
- 无 provider、无登记、未知页面均得到 owner={0,0}。已有新后端写接口拒绝未知
  owner。现有普通 zRAM 和 legacy writeback 不受未知身份阻止。
- 目前 registry 无数量硬上限，登记方须在 memcg 生命周期结束时注销；
  对外开放接口前必须增加权限、配额及生命周期接入。

## 目标设备接入前必须确认
1. 配套核心内核版本、MEMCG 配置、swap page 的实际记账获取 API 和模块可见性。
2. memory controller 实际层级：是否启用按应用/进程分组，能否可靠映射完整 UID。
3. provider 如何得到生命周期唯一 cookie；谁登记/注销，以及如何管理应用代次。
4. 若 memory cgroup 混合多个应用，不允许凭 current UID 或 cgroup 名称猜测。
   需要更早的归属采集或 Android 分组/登记支持。
5. CPU/EH、bio/rw_page、失败、部分 I/O、reset 路径的目标内核验证。
6. 真正的分组扫描、压缩下沉 slot 交接、引用保护、GC 仍未实现。

## 成本与测试
开启 CONFIG_ZRAM_SEGMENT_BACKEND 后，每个 zRAM 逻辑 slot 增加 16 字节 owner，
每个压缩 cookie 增加 16 字节；cookie 当前即使配置关闭仍有该字段。
按 4 KiB 页、8 GiB 逻辑 zRAM 容量举例，slot owner 本身约增加 32 MiB。
registry 每设备一个小哈希表，每个登记另分配一个节点；这是后续需核算的成本。
不为 owner 在每次压缩时另行分配内存。

新增 zram-owner KUnit：同 UID 多 memcg 聚合、不同 Android 用户分离、身份代次、
冲突登记、未知归属、注销不修改已保存身份、普通临时页拒绝、槽位覆盖清理。
测试未运行。需要实际内核验证 CONFIG_ZRAM_SEGMENT_BACKEND 开/关、CPU/EH 组合；
KUnit 延用内建 ZRAM_GS=y 配置。正向真实 memcg page 采集需目标适配器集成测试。
