# mini-lsm

从零实现一个 LSM 存储引擎，逐步接上分布式（Raft）。学习/作品项目。

## 构建与测试

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

首次构建会通过 CMake FetchContent 拉取 googletest（需要网络）。

## 进度

- [x] commit 1：项目骨架（CMake + googletest + Slice/Status + 空测试）
- [ ] commit 2：CRC32C
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
