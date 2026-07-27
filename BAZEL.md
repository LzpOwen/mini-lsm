# mini-lsm 的 Bazel 配置说明

本项目 CMake 与 Bazel 两套构建系统并存。这份文档讲清楚 Bazel 侧的每个配置文件及其作用。

## 全景

五个文件各管一段：

| 文件 | 管什么 | 类比 CMake |
|------|--------|-----------|
| `.bazelversion` | 用哪个版本的 bazel | 无对应（类似锁 cmake 版本） |
| `MODULE.bazel` | 外部依赖（拉 googletest） | FetchContent 那段 |
| `BUILD.bazel` | 本项目怎么编译成库和测试 | add_library / add_executable |
| `.bazelrc` | 编译的全局默认参数 | CMAKE_CXX_FLAGS 等 |
| `.bazelignore` | 哪些目录 bazel 别扫 | 无对应 |

---

## 1. `.bazelversion` — 锁 bazel 版本

```
7.4.1
```

一行版本号。它不是 bazel 本体的功能，而是 **bazelisk** 读取的。

bazelisk 是 bazel 的"版本管理器 + 启动器"（本项目 VM 里装的 `bazel` 命令其实就是 bazelisk）。执行 `bazel test` 时，它先看当前目录有没有 `.bazelversion`，有就下载/使用那个精确版本再转发命令。

**为什么重要**：bazel 各大版本行为差异大（尤其 bzlmod 在 6/7/8 之间变化很多）。锁死版本 = 本机、同事机器、CI 上跑的都是同一个 bazel，杜绝"我这能编你那编不了"。首次 `bazel test` 会先下载几十 MB，就是 bazelisk 在拉对应版本的 bazel 本体。

---

## 2. `MODULE.bazel` — 依赖声明（bzlmod 的核心）

```python
module(
    name = "mini_lsm",
    version = "0.1.0",
)

bazel_dep(name = "googletest", version = "1.14.0")
```

这是新旧 bazel 的分水岭。老式 bazel 用 `WORKSPACE` 文件，手写一堆 `http_archive` 下载依赖，还要自己管依赖的依赖。**bzlmod（`MODULE.bazel`）** 是新机制，像 `package.json` / `Cargo.toml` / `go.mod` 那样声明式管依赖。

逐行：

- **`module(name, version)`**：声明"我自己"是一个模块，叫 `mini_lsm`，版本 0.1.0。将来别人依赖你，就用这个名字。当前阶段主要是自我标识。
- **`bazel_dep(name = "googletest", version = "1.14.0")`**：声明依赖 googletest 1.14.0。

关键点：**这个名字和版本不是随便写的，bazel 拿它去 Bazel Central Registry（BCR，registry.bazel.build）查。** BCR 是官方中央仓库（类比 Maven Central、npm registry），登记了每个模块在哪下载、它自己又依赖什么。所以你只写一行 `googletest`，bazel 会自动把它的间接依赖一并解析下来，不用你操心依赖树。这是 bzlmod 比老 WORKSPACE 省心的地方。

**和 CMake 对照**：`CMakeLists.txt` 里那段 `FetchContent_Declare(googletest ...)` 干同一件事——拉 googletest。区别是 CMake 写死了 git 地址和 tag，bzlmod 只报名字让 BCR 去解析。

副产物 `MODULE.bazel.lock`（本项目 gitignore 了，也可选择提交）：解析后的锁文件，记录每个依赖的精确 hash，类比 `package-lock.json`。提交它能让构建完全可复现；不提交则每次按 registry 重新解析。

---

## 3. `BUILD.bazel` — 构建目标定义

```python
package(default_visibility = ["//visibility:public"])

cc_library(
    name = "mlsm",
    srcs = glob(["src/*.cc"]),
    hdrs = glob(["include/mlsm/*.h"]),
    includes = ["include"],
)

cc_test(
    name = "mlsm_test",
    srcs = glob(["test/*.cc"]),
    deps = [
        ":mlsm",
        "@googletest//:gtest_main",
    ],
)
```

最核心的文件，定义"编什么、怎么编"。bazel 里每个 `BUILD.bazel` 定义一个 **package**，里面是一个个 **target**（构建目标）。

**`package(default_visibility = ["//visibility:public"])`**
把本包所有 target 默认可见性设为"公开"——任何其他包都能依赖。bazel 默认私有（只有同包能用），对多模块大项目这种严格可见性是好事（强制显式声明依赖），但对单包小项目设成 public 省得处处写 `visibility`。

**`cc_library(name = "mlsm", ...)`** — 编译核心引擎库
- **`name = "mlsm"`**：target 名。别人用 `//:mlsm` 引用（`//` 是仓库根，`:mlsm` 是根包下这个 target）。
- **`srcs = glob(["src/*.cc"])`**：源文件。`glob` 通配，自动匹配 `src/` 下所有 `.cc`。**这就是"加新模块 bazel 侧不用改"的原因**——新增 `crc32c.cc`，glob 自动收进来。（对比 CMake 那边要手动往 `add_library` 里加文件名，因为 CMake 官方不推荐 glob 源文件：glob 在 CMake 里改文件不会触发重新配置，容易漏编。bazel 的 glob 是构建时求值，没这个坑。）
- **`hdrs = glob(["include/mlsm/*.h"])`**：头文件。`hdrs` 声明"这个库对外暴露的头"，依赖它的 target 才能 `#include` 这些头。
- **`includes = ["include"]`**：把 `include/` 加进头文件搜索路径（相当于 `-Iinclude`）。所以代码里能写 `#include "mlsm/slice.h"` 而不是带 `include/` 前缀。

**`cc_test(name = "mlsm_test", ...)`** — 编译并注册测试
- **`srcs = glob(["test/*.cc"])`**：测试源文件，自动匹配 `test/*.cc`。
- **`deps`**：依赖列表，bazel 的精髓——**显式声明依赖图**：
  - **`":mlsm"`**：依赖上面那个库（同包内用 `:name` 引用）。测试因此能用到 `Slice`、`Status`。
  - **`"@googletest//:gtest_main"`**：依赖外部模块 googletest 里的 `gtest_main` target。`@googletest` 的 `@` 表示"外部仓库"（即 `MODULE.bazel` 里 `bazel_dep` 拉进来的那个），`//:gtest_main` 是它内部提供的 target（自带 `main()`，所以测试文件不用自己写 main）。

`cc_test` 特殊在：不只编译，还把产物注册成"可被 `bazel test` 运行的测试"。所以 `bazel test //...` 能自动发现并跑它。

---

## 4. `.bazelrc` — 全局默认参数

```
build --cxxopt=-std=c++17
build --host_cxxopt=-std=c++17
build --copt=-Wall
build --copt=-Wextra
common --enable_bzlmod
test --test_output=errors
```

bazel 的配置文件，作用是**给命令预置默认 flag**，省得每次命令行敲一长串。每行格式 `<命令> <flag>`：

- **`build --cxxopt=-std=c++17`**：`build` 默认加 `-std=c++17`。`cxxopt` 只作用于 C++ 编译。因为 `test` 底层也先 `build`，测试也吃这个 flag。对应 CMake 的 `set(CMAKE_CXX_STANDARD 17)`。
- **`build --host_cxxopt=-std=c++17`**：`host_cxxopt` 作用于"host 工具"的编译。bazel 有 target/host 之分——为构建过程本身跑的工具（代码生成器等）算 host。保持一致，避免某些工具用了更低默认标准报错。
- **`--copt=-Wall` / `--copt=-Wextra`**：`copt` 是通用编译选项（C 和 C++ 都吃），开启告警。对齐 CMake 的 `add_compile_options(-Wall -Wextra)`。
- **`common --enable_bzlmod`**：`common` 对所有命令生效，显式打开 bzlmod。bazel 7 默认就开，写这行是为了明确表意与向后兼容。
- **`test --test_output=errors`**：只给 `test` 用，含义"测试失败时才打印详细日志"。默认通过只显示 PASSED、失败也只给摘要；这行让失败时直接打出测试输出，方便定位。

**为什么用 rc 而不是命令行**：团队所有人、CI 跑的都是同一套编译参数，不依赖各人记不记得敲 flag。这是 bazel"构建可复现"哲学的一部分。

---

## 5. `.bazelignore` — 排除目录

```
build
cmake-build-debug
cmake-build-release
```

告诉 bazel "扫描 package 时跳过这几个目录"。

**为什么需要它**（本项目踩过的坑）：`bazel test //...` 里的 `//...` 表示"递归扫描所有目录，找出全部 target"。而 CMake 构建时会在 `build/` 里通过 FetchContent 拉一份 googletest 源码，那份源码**自带 `BUILD.bazel`**。bazel 扫到它，试图解析这些不属于本项目的 build 文件，撞上环境里不存在的配置键，直接报错 `Build did NOT complete successfully`。

加上 `.bazelignore` 后，bazel 扫描时跳过 `build/`，只看真正的源码。

> 注意：`.gitignore` 是给 git 看的，`.bazelignore` 是给 bazel 看的，两个独立系统，都要各自忽略 `build/`。

---

## 串起来：一次 `bazel test //...` 发生了什么

```
1. bazelisk 读 .bazelversion → 确保用 bazel 7.4.1
2. bazel 读 .bazelrc → 预加载 -std=c++17 -Wall -Wextra 等默认 flag
3. bazel 读 MODULE.bazel → 发现依赖 googletest 1.14.0 → 去 BCR 解析并下载
4. //... 触发扫描所有目录，.bazelignore 让它跳过 build/
5. 读 BUILD.bazel → 构建依赖图：mlsm_test 依赖 mlsm 和 @googletest
6. 按图编译 mlsm 库 → 编译 mlsm_test → 链接 gtest_main
7. 运行 mlsm_test，test_output=errors 决定输出详略 → PASSED
```

---

## 后续演进方向

现在 `cc_library` 用 glob 图省事，但大型项目里更推荐**细粒度 target**（一个模块一个库），编译缓存更精准、依赖图更清晰。等模块多起来（WAL、memtable、sstable 各成一库），可以把 `mlsm` 拆成 `wal`、`memtable` 等独立 `cc_library`，届时能更真切体会 bazel 依赖图的价值。
