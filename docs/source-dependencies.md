# 配套源码线索（只读检查，2026-10-17）

本仓库顶层为 devices、google-modules 及项目新增 docs/tools。
以下是构建引用，不代表用户已选定 shusky，也不能据此确定核心内核提交。

## 已确认引用
devices/google/shusky/BUILD.bazel：
- 第 19 行引用 //common:modules.bzl。
- 第 26 行引用 //common:set_kernel_dir_build_config。
- 第 139 行引用 //common:kernel_aarch64_sources。
- 第 160 行 base_kernel = //common:kernel_aarch64_download_or_build。
- 第 165 行引用 //common:android/abi_gki_aarch64_pixel。
- 第 6、8 行引用 //build/kernel/kleaf。

devices/google/shusky/build.config.shusky：
引入 private/devices/google/zuma/build.config.zuma，并选择 shusky_gki.fragment。

devices/google/zuma/build.config.zuma：
引用 ROOT_DIR/KERNEL_DIR 下的 build.config.common、build.config.aarch64、
scripts/kconfig/merge_config.sh、arch/arm64/configs/gki_defconfig。

因此 private 是较大工作区中的设备/厂商源码部分，不是完整核心内核树。
缺少 manifest/锁定提交证据时，不能猜测 common 的分支或拿任意上游版本替代。

## 应用归属线索
当前 vendor cgroup hook 位于
google-modules/soc/gs/drivers/soc/google/vh/kernel/cgroup/。
检查到的 hook 处理内核线程迁移，不提供现成 App UID -> memcg 登记。
当前仓库未找到 Android system/core/libprocessgroup、
对应核心 mm/memcontrol.c 或 Android memory cgroup 配置的完整来源。

后续需要：配套 common 仓库/commit、Android 版本与 libprocessgroup/cgroup 配置、
实际构建目标与配置。当前未知不阻止纯后端算法开发。

## 已记录的 F2FS 任务
按学长提出的方向，缩小 F2FS 描述的有效范围，永久排除尾部地址区间，
再将该区间映射成 zRAM 后端块设备。
待核对配套 F2FS/格式化工具及布局一致性；没有调整任何分区、文件系统或设备。
