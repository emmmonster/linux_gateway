#!/bin/sh
# 守护脚本：负责启动、失败回滚、崩溃重启。
# 用法：./run.sh [sim|uart] [broker] [topic]
#
# 分工：
#   本脚本  —— 判断“新版本起不来”并回滚（新版本自己做不到）
#   应用    —— 跑满 10 秒后自己确认升级（它知道业务是否健康）

set -u

cd "$(dirname "$0")" || exit 1

PENDING=app/pending
ATTEMPTED=app/attempted
CONFIRM_SEC=10

# 1. 上次升级没有确认就又要启动 —— 说明新版本没跑起来，回滚
if [ -f "$PENDING" ] && [ -f "$ATTEMPTED" ]; then
    newv=$(awk '{print $1}' "$PENDING")
    oldv=$(awk '{print $2}' "$PENDING")
    if [ -n "$oldv" ] && [ -d "app/$oldv" ]; then
        ln -sfn "$oldv" app/current
        rm -f "$PENDING" "$ATTEMPTED"
        echo "[watchdog] 新版本 $newv 未确认，已回滚到 $oldv" >&2
    fi
fi

# 2. 记录“这次尝试过了新版本”
if [ -f "$PENDING" ]; then
    : > "$ATTEMPTED"
fi

# 3. 启动
start=$(date +%s)
./app/current/gateway "$@"
rc=$?
end=$(date +%s)

# 4. 跑够 CONFIRM_SEC 秒就算成功（应用自己也会确认，这里兜底）
if [ -f "$PENDING" ] && [ $((end - start)) -ge "$CONFIRM_SEC" ]; then
    rm -f "$PENDING" "$ATTEMPTED"
    echo "[watchdog] 运行满 ${CONFIRM_SEC} 秒，升级确认" >&2
fi

exit "$rc"
