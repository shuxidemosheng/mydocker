#!/bin/bash
# scripts/verify-stage5.sh —— 阶段 5 管理链路验收：run(后台) -> ps -> exec -> rm
# 在仓库根目录以 root 执行：sudo ./scripts/verify-stage5.sh
set -u
cd "$(dirname "$0")/.."

# 1. 后台起一个 20 秒的容器
printf '/bin/sleep 20; exit\n' | ./mydocker run \
    --rootfs "$PWD/rootfs" --overlay --net --hostname web >/dev/null 2>&1 &

sleep 2

# 2. ps 发现容器
echo "=== mydocker ps ==="
./mydocker ps
PID=$(./mydocker ps | awk 'NR>1 {print $1; exit}')
[ -z "$PID" ] && { echo "FAIL: ps 未发现容器"; exit 1; }
echo "found container pid=$PID"

# 3. exec 进入容器，验证隔离环境
echo "=== mydocker exec $PID ==="
printf 'hostname; /bin/ip -4 addr show eth0 | grep inet; /bin/ps\n' \
    | ./mydocker exec "$PID"
RC=$?
[ $RC -ne 0 ] && { echo "FAIL: exec 退出码 $RC"; exit 1; }

# 4. rm 终止容器
echo "=== mydocker rm $PID ==="
./mydocker rm "$PID"
sleep 1
./mydocker ps
echo "PASS: stage5 验收通过"
