#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# 清理按日期命名的日志。默认删除 7 天前的，传 all 删除全部。
days="${1:-7}"
case "${days}" in
  all)
    rm -f logs/qaiservice-*.log
    echo "已删除全部日志"
    ;;
  *)
    find logs -name 'qaiservice-*.log' -mtime +"${days}" -delete
    echo "已删除 ${days} 天前的日志"
    ;;
esac
