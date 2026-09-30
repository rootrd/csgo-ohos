#!/usr/bin/env bash
PID=$1
echo "=== threads: state wchan utime+stime name ==="
for t in /proc/$PID/task/*; do
  tid=$(basename $t)
  name=$(cat $t/comm 2>/dev/null)
  # 去掉 "pid (comm) " 前缀后：state=$1, utime=$12, stime=$13（相对字段）
  rest=$(sed 's/^[0-9]* ([^)]*) //' $t/stat 2>/dev/null)
  set -- $rest
  state=$1
  ut=$13
  st=$14
  wchan=$(cat $t/wchan 2>/dev/null)
  echo "$((ut+st)) $state $wchan $tid $name"
done | sort -rn | head -16
