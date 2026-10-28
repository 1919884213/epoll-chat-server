#!/bin/bash
# chatsnap x 聊天服务器：端到端集成验收。用法（在仓库根目录）：
#   sudo MOD=~/chatsnap-build/chatsnap/chatsnap.ko bash scripts/chatsnap_integ.sh
U="${SUDO_USER:-$(id -un)}"; H="$(getent passwd "$U" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
MOD="${MOD:-$H/chatsnap-build/chatsnap/chatsnap.ko}"
pass=0; fail=0
ok(){ echo "  [PASS] $1"; pass=$((pass+1)); }
no(){ echo "  [FAIL] $1"; fail=$((fail+1)); }

[ -f "$MOD" ] || { echo "[FAIL] 没有模块文件 $MOD"; exit 1; }
insmod "$MOD" buf_kb=64 && ok "驱动加载" || { echo "insmod 失败"; exit 1; }
sleep 0.3

"$HERE/build/ChatSnapMon" > /tmp/integ_snap.log 2>&1 &
MONPID=$!
sleep 0.3

stdbuf -o0 -e0 "$HERE/build/Pro" > /tmp/integ_pro.log 2>&1 &
PROPID=$!
sleep 1
grep -q "聊天快照通道 -> /dev/chatsnap" /tmp/integ_pro.log && ok "服务器已接上快照通道" || no "服务器没接上通道"
sed 's/^/    /' /tmp/integ_pro.log

echo "===== 真客户端：进房间 + 发言（唯一会产生广播的路径）====="
python3 - <<'PY'
import socket, time
s = socket.create_connection(("127.0.0.1", 8080), timeout=3)
s.settimeout(3)
time.sleep(0.4); s.sendall(b"itest\n")
time.sleep(0.4); s.sendall(b"JOIN lobby\n")
time.sleep(0.4); s.sendall(b"hello from integration test\n")
time.sleep(0.6); s.close()
print("  客户端已发送：昵称 -> JOIN lobby -> 消息")
PY
sleep 0.5

ST=$("$HERE/build/ChatSnapMon" -s); echo "    驱动统计: $ST"
echo "$ST" | grep -qE "in=[1-9]" && ok "驱动确实收到写入（in>0）" || no "驱动 in=0，没有数据进来"
grep -q "hello from integration test" /tmp/integ_snap.log && ok "监控端从内核读回聊天内容（端到端成立）" || no "监控端没读到内容"
echo "  --- 监控端抓到的内容 ---"; sed 's/^/    /' /tmp/integ_snap.log | head -10

echo "===== 收尾 ====="
kill $PROPID $MONPID 2>/dev/null; wait 2>/dev/null
rmmod chatsnap && ok "卸载干净" || no "卸载失败"

echo; echo "===== 汇总：PASS $pass / FAIL $fail ====="
