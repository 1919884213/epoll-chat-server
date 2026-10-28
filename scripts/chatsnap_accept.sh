#!/bin/bash
# chatsnap 驱动验收（12 项）。用法：
#   sudo MOD=~/chatsnap-build/chatsnap/chatsnap.ko bash scripts/chatsnap_accept.sh
# 注意：sudo 下 $HOME=/root，所以按 SUDO_USER 反查真实家目录，别用 $HOME 拼路径。
U="${SUDO_USER:-$(id -un)}"; H="$(getent passwd "$U" | cut -d: -f6)"
HERE="$(cd "$(dirname "$0")" && pwd)"
MOD="${MOD:-$H/chatsnap-build/chatsnap/chatsnap.ko}"
MON="$HERE/build/ChatSnapMon"
DEV=/dev/chatsnap
pass=0; fail=0
ok(){ echo "  [PASS] $1"; pass=$((pass+1)); }
no(){ echo "  [FAIL] $1"; fail=$((fail+1)); }

[ -f "$MOD" ] || { echo "[FAIL] 没有模块文件 $MOD （先在它的目录 make -C /lib/modules/$(uname -r)/build M=$PWD modules）"; exit 1; }
if [ -e "$DEV" ] && [ ! -c "$DEV" ]; then echo "  清掉残留的非字符设备文件 $DEV"; rm -f "$DEV"; fi

echo "===== 1. 加载 ====="
insmod "$MOD" buf_kb=64 && ok "insmod 成功" || { no "insmod 失败"; exit 1; }
sleep 0.3
[ -c "$DEV" ] && ok "/dev/chatsnap 是字符设备（udev 自动创建）" || no "没有字符设备节点"
dmesg | tail -5 | grep -q chatsnap && ok "dmesg 有加载日志" || no "无加载日志"
dmesg | tail -5 | grep chatsnap | sed 's/^/    /'

echo "===== 2. sysfs 属性 ====="
[ -r /sys/class/chatsnap/chatsnap/stat ] && { cat /sys/class/chatsnap/chatsnap/stat; ok "sysfs stat 可读"; } || no "sysfs stat 不可读"

echo "===== 3. 双向数据（阻塞读）====="
rm -f /tmp/snap_read.log
timeout 5 cat $DEV > /tmp/snap_read.log &
sleep 0.5; echo "hello-from-producer" > $DEV; sleep 0.5
grep -q hello-from-producer /tmp/snap_read.log && ok "生产者 write -> 消费者 read 收到" || no "消费者没收到数据"
wait 2>/dev/null

echo "===== 4. 空缓冲时阻塞（应超时 rc=124）====="
timeout 2 cat $DEV > /dev/null; rc=$?
[ "$rc" = "124" ] && ok "阻塞语义正确（rc=124，不是忙等）" || no "非预期返回 rc=$rc"

echo "===== 5. 溢出保护 + drop 计数 ====="
for i in $(seq 1 20000); do echo "line-$i"; done > $DEV
S=$(cat /sys/class/chatsnap/chatsnap/stat); echo "    $S"
echo "$S" | grep -qE "drop=[1-9]" && ok "缓冲满整条丢弃并计数（drop>0）" || no "drop 没增长"
dmesg | tail -3 | grep -qiE "oops|BUG:|WARNING:" && no "内核日志出现 oops/BUG/WARNING" || ok "无 oops/BUG/WARNING"

echo "===== 6. ioctl（统计 / 清空）====="
if [ -x "$MON" ]; then
  "$MON" -s | sed 's/^/    /'
  "$MON" -c && ok "CHATSNAP_CLEAR 成功"
  "$MON" -s | sed 's/^/    /'
else
  echo "  [SKIP] 监控端不存在：$MON"
fi

echo "===== 7. 卸载语义（有 fd 占用应 EBUSY）====="
exec 9< $DEV
OUT=$(rmmod chatsnap 2>&1); echo "    rmmod（仍被占用）: $OUT"
echo "$OUT" | grep -qiE "busy|in use" && ok "占用时报 EBUSY（try_module_get 生效）" || no "占用时竟然卸载成功"
exec 9<&-
rmmod chatsnap && ok "释放后 rmmod 成功" || no "释放后仍卸不掉"
[ -e "$DEV" ] && no "卸载后 $DEV 仍存在" || ok "节点已清理"

echo; echo "===== 汇总：PASS $pass / FAIL $fail ====="
