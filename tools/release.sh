#!/bin/bash
# coffe_mcu 一键发布：提交 → 打 tag → 推送 GitHub → 同步 NAS
#
# 用法：tools/release.sh v1.2.2 "本次变更摘要"
#   - 版本号与 CMakeLists.txt 的 VERSION、CHANGELOG.md 顶部段落对应
#   - 先手动改好 VERSION 并在 CHANGELOG.md 顶部补一段说明，再跑本脚本
set -e
cd "$(dirname "$0")/.."

V="${1:?用法: tools/release.sh vX.Y.Z \"摘要\"}"
MSG="${2:?缺少变更摘要}"

# 一致性检查：CMakeLists VERSION 应与 tag 号一致（去掉前缀 v）
CUR="$(sed -nE 's/^project\(coffe_mcu VERSION ([0-9.]+)\).*/\1/p' CMakeLists.txt)"
if [ "${V#v}" != "$CUR" ]; then
    echo "警告: tag $V 与 CMakeLists VERSION=$CUR 不一致（继续执行）"
fi

git add -A
git commit -m "release: $V — $MSG"
git tag -a "$V" -m "coffe_mcu $V: $MSG"
git push origin main --tags

NAS="/Volumes/Nas_AlvinX/nas_alvinx/开发/开发项目/无界coffe"
if [ -d "$NAS" ]; then
    rsync -a --delete --exclude .git --exclude build --exclude sdkconfig \
          --exclude sdkconfig.old --exclude managed_components ./ "$NAS/"
    echo "NAS synced -> $NAS"
else
    echo "NAS 未挂载，跳过同步"
fi

echo "发布完成: $V"
