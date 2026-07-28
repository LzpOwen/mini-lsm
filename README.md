# mini-lsm

从零实现一个 LSM 存储引擎，逐步接上分布式（Raft）。学习/作品项目。

## 构建与测试

两套构建系统并存，任选其一。

### CMake

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

首次构建会通过 CMake FetchContent 拉取 googletest（需要网络）。

### Bazel（bzlmod，对齐线上 taishan-bzlmod）

```bash
bazel test //...     # 编译 + 跑测试
bazel build //...    # 只编译
```

依赖通过 `MODULE.bazel` 从 Bazel Central Registry 拉取。构建产物在 `bazel-*` 软链下（已 gitignore）。

### 在 VM 里构建（推荐）

代码在 Mac 编辑，通过 `sync.sh` 增量同步到 `lsm-lab` VM 编译：

```bash
./sync.sh test     # CMake: 同步 + 编译 + 测试
./sync.sh bazel    # Bazel: 同步 + bazel test //...
./sync.sh shell    # 进 VM 交互 shell
```

## 进度

- [x] commit 1：项目骨架（CMake + googletest + Slice/Status + 空测试）
- [x] commit 2：CRC32C（Castagnoli 多项式，含 Mask/Unmask，对齐 LevelDB/RocksDB）
- [ ] commit 3：WAL Writer（record 格式，按 32KB block 切分）
- [ ] commit 4：WAL Reader（读回 + crc 校验 + 残缺 record 识别）
- [ ] commit 5：DB 接口 + WAL 打通写路径 + 崩溃恢复
- [ ] commit 6：benchmark 骨架

## 布局

```
include/mlsm/   公共头文件
src/            实现
test/           单测
```
