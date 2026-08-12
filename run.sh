#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

usage()
{
  echo "用法: ./run.sh [start|stop|down]"
  echo "  start   启动 server"
  echo "  stop    停止全部服务（数据保留）"
  echo "  down    停止并删除容器（数据卷保留）"
}

case "${1:-start}" in
  start)
    docker compose --profile rag up -d --build server
    ;;
  stop)
    docker compose --profile rag stop
    ;;
  down)
    docker compose --profile rag down
    ;;
  *)
    usage
    exit 1
    ;;
esac
