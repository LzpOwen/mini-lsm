#!/usr/bin/env bash
#
# sync.sh — 把本地 mini-lsm 源码增量同步到 lsm-lab VM，可选顺带编译/测试。
#
# 用法:
#   ./sync.sh              只同步源码
#   ./sync.sh build        同步 + 在 VM 里用 CMake 编译
#   ./sync.sh test         同步 + CMake 编译 + 跑测试
#   ./sync.sh bazel        同步 + 在 VM 里用 bazel 编译+测试 (bazel test //...)
#   ./sync.sh shell        同步 + 进入 VM 交互 shell（cd 到项目目录）
#
# 依赖: 本机与 VM 均有 rsync；已配置 ssh 免密到 ubuntu@<vm-ip>。

set -euo pipefail

VM_NAME="lsm-lab"
REMOTE_USER="ubuntu"
REMOTE_DIR="/home/ubuntu/mini-lsm"
LOCAL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 动态取 VM IP（重启后 IP 可能变）
VM_IP="$(multipass info "$VM_NAME" | awk '/IPv4/{print $2}')"
if [[ -z "$VM_IP" ]]; then
  echo "!! 拿不到 $VM_NAME 的 IP，VM 是否在运行？(multipass start $VM_NAME)" >&2
  exit 1
fi

REMOTE="${REMOTE_USER}@${VM_IP}"

echo ">> 同步 ${LOCAL_DIR}/  ->  ${REMOTE}:${REMOTE_DIR}/"
# --delete: 让 VM 端与本地保持一致（删掉本地已删除的文件）
# 排除 build 产物、git 元数据、编辑器杂物
rsync -az --delete \
  --exclude 'build/' \
  --exclude '.git/' \
  --exclude '.cache/' \
  --exclude 'compile_commands.json' \
  --exclude '.DS_Store' \
  -e "ssh -o StrictHostKeyChecking=accept-new" \
  "${LOCAL_DIR}/" "${REMOTE}:${REMOTE_DIR}/"
echo ">> 同步完成"

ACTION="${1:-}"
case "$ACTION" in
  build)
    ssh "$REMOTE" "cd $REMOTE_DIR && cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null && cmake --build build"
    ;;
  test)
    ssh "$REMOTE" "cd $REMOTE_DIR && cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null && cmake --build build && ctest --test-dir build --output-on-failure"
    ;;
  bazel)
    ssh "$REMOTE" "cd $REMOTE_DIR && bazel test //..."
    ;;
  shell)
    ssh -t "$REMOTE" "cd $REMOTE_DIR && exec bash"
    ;;
  "")
    ;;
  *)
    echo "未知参数: $ACTION  (可用: build | test | bazel | shell)" >&2
    exit 1
    ;;
esac
