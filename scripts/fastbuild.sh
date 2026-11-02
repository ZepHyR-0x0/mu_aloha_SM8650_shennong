#!/usr/bin/env bash
#
# 兼容旧入口：等价于仓库根目录的 build.sh [DEBUG|RELEASE]。
# 不再依赖 /mnt/l 或 /root/mubuild 等本机路径。
#
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build.sh" "$@"
