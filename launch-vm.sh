#!/usr/bin/env bash
#
# launch-vm.sh — 随时把 lsm-lab VM 拉起来，确认可达并打印工具链，可顺带跑验证。
#
# 用法:
#   ./launch-vm.sh            确保 VM 在运行 + 打印 IP/工具链版本
#   ./launch-vm.sh test       拉起 VM 后接 ./sync.sh test（同步+编译+测试）
#   ./launch-vm.sh build      拉起 VM 后接 ./sync.sh build
#   ./launch-vm.sh shell      拉起 VM 后进入项目目录交互 shell
#   ./launch-vm.sh info       只打印 VM 状态与工具链，不做别的
#
# 与 sync.sh 配套：本脚本负责“把机器弄到能用的状态”，sync.sh 负责“把代码送上去跑”。
# 依赖: 本机装了 multipass、rsync；已配置 ssh 免密到 ubuntu@<vm-ip>。

set -euo pipefail

VM_NAME="lsm-lab"
REMOTE_USER="ubuntu"
REMOTE_DIR="/home/ubuntu/mini-lsm"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 等 VM 拿到 IP 的最长秒数（冷启动可能要十几秒）。
BOOT_TIMEOUT=60

vm_state() { multipass info "$VM_NAME" 2>/dev/null | awk '/^State:/{print $2}'; }
vm_ip()    { multipass info "$VM_NAME" 2>/dev/null | awk '/IPv4/{print $2}'; }

# ---- 1. VM 必须存在 ----
if ! multipass info "$VM_NAME" >/dev/null 2>&1; then
  echo "!! 找不到名为 '$VM_NAME' 的 VM。先创建它，或改脚本顶部的 VM_NAME。" >&2
  echo "   现有实例：" >&2
  multipass list >&2 || true
  exit 1
fi

# ---- 2. 没运行就拉起来 ----
STATE="$(vm_state)"
if [[ "$STATE" != "Running" ]]; then
  echo ">> $VM_NAME 当前状态：${STATE:-未知}，正在启动…"
  multipass start "$VM_NAME"
else
  echo ">> $VM_NAME 已在运行"
fi

# ---- 3. 等到拿到 IP ----
VM_IP=""
for ((i = 0; i < BOOT_TIMEOUT; i++)); do
  VM_IP="$(vm_ip)"
  [[ -n "$VM_IP" ]] && break
  sleep 1
done
if [[ -z "$VM_IP" ]]; then
  echo "!! 等了 ${BOOT_TIMEOUT}s 还没拿到 IP，VM 可能没起来。" >&2
  exit 1
fi
REMOTE="${REMOTE_USER}@${VM_IP}"

# ---- 4. 等 ssh 真正可达 ----
echo ">> 等待 ssh 可达 ($REMOTE) …"
for ((i = 0; i < BOOT_TIMEOUT; i++)); do
  if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=3 \
         -o BatchMode=yes "$REMOTE" true 2>/dev/null; then
    break
  fi
  sleep 1
done

# ---- 5. 打印工具链，确认这台机器能编译 ----
echo ">> ${VM_NAME} 就绪 @ ${VM_IP}，工具链："
ssh -o StrictHostKeyChecking=accept-new "$REMOTE" '
  . /etc/os-release 2>/dev/null
  printf "   %-8s %s\n" "OS:"    "$PRETTY_NAME ($(uname -m))"
  printf "   %-8s %s\n" "cmake:" "$(cmake --version 2>/dev/null | head -1 | sed "s/cmake version //")"
  printf "   %-8s %s\n" "ninja:" "$(ninja --version 2>/dev/null || echo n/a)"
  printf "   %-8s %s\n" "g++:"   "$(g++ -dumpfullversion 2>/dev/null || echo n/a)"
  printf "   %-8s %s\n" "git:"   "$(git --version 2>/dev/null | sed "s/git version //")"
'

# ---- 6. 可选：接力到 sync.sh 做验证 ----
ACTION="${1:-}"
case "$ACTION" in
  test | build | shell | bazel)
    echo ">> 接力：./sync.sh $ACTION"
    exec "$SCRIPT_DIR/sync.sh" "$ACTION"
    ;;
  info | "")
    echo ">> VM 已就绪。跑验证：./sync.sh test  或  ./launch-vm.sh test"
    ;;
  *)
    echo "未知参数: $ACTION  (可用: test | build | shell | bazel | info)" >&2
    exit 1
    ;;
esac
