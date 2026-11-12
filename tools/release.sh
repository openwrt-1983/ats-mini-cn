#!/usr/bin/env bash
#
# Build ats-mini-cn and publish it as a GitHub release.
#
# The tag carries the iteration, the file names never do:
#
#   tools/release.sh          -> tag ats-mini-cn_v2.45    (baseline)
#   tools/release.sh 1        -> tag ats-mini-cn_v2.45-1  (first source change)
#   tools/release.sh 2        -> tag ats-mini-cn_v2.45-2
#
# Every release ships the same five attachments, always with the same names,
# so download links, tutorials and flashing scripts keep working across
# versions:
#
#   ats-mini-cn_v2.45_app_0x010000.bin
#   ats-mini-cn_v2.45_boot_app0_0x00e000.bin
#   ats-mini-cn_v2.45_bootloader_0x000000.bin
#   ats-mini-cn_v2.45_partitions_0x008000.bin
#   ats-mini-cn_v2.45_merged_full8MB_0x000000.bin
#
# The firmware version inside the image stays v2.45 (VER_APP 245); the
# iteration lives only in the tag and the release title.
#
# Environment:
#   GITHUB_TOKEN   required, token with repo scope
#   GITHUB_REPO    default openwrt-1983/ats-mini-cn
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${GITHUB_REPO:-openwrt-1983/ats-mini-cn}"
BASE="ats-mini-cn_v2.45"
API="https://api.github.com"

ITER=""
FORCE=0
SKIP_BUILD=0

PYTHON="${PYTHON:-python3}"

usage() {
  cat <<'USAGE'
usage: tools/release.sh [-f|--force] [-s|--skip-build] [iteration]

  iteration   omitted -> tag ats-mini-cn_v2.45 (baseline)
              1       -> tag ats-mini-cn_v2.45-1
              2       -> tag ats-mini-cn_v2.45-2, and so on

  -f, --force       replace an existing tag and release
  -s, --skip-build  reuse out/ and build/ instead of rebuilding

Needs GITHUB_TOKEN (repo scope). GITHUB_REPO overrides the default owner/name.
USAGE
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    -f|--force)      FORCE=1; shift ;;
    -s|--skip-build) SKIP_BUILD=1; shift ;;
    -h|--help)       usage 0 ;;
    --)              shift; break ;;
    -*)              echo "unknown option: $1" >&2; usage 1 ;;
    *)               ITER="$1"; shift ;;
  esac
done

if [ -n "$ITER" ] && ! [[ "$ITER" =~ ^[1-9][0-9]*$ ]]; then
  echo "iteration must be a positive integer, got: $ITER" >&2
  exit 1
fi

TAG="$BASE"
[ -n "$ITER" ] && TAG="$BASE-$ITER"

: "${GITHUB_TOKEN:?GITHUB_TOKEN is not set}"

api() {
  local method="$1" url="$2"; shift 2
  curl -sS -X "$method" \
    -H "Authorization: token $GITHUB_TOKEN" \
    -H "Accept: application/vnd.github+json" \
    "$@" "$url"
}

json() { "$PYTHON" -c "import sys,json;d=json.load(sys.stdin);print($1)"; }

echo "==> repository $REPO"
api GET "$API/repos/$REPO" > /tmp/rel_repo.json
REPO_ID="$(json "d.get('id','')" < /tmp/rel_repo.json)"
if [ -z "$REPO_ID" ]; then
  echo "cannot read repository, check GITHUB_TOKEN" >&2
  cat /tmp/rel_repo.json >&2
  exit 1
fi

# ---------------------------------------------------------------- build -----
if [ "$SKIP_BUILD" -eq 0 ]; then
  echo "==> build"
  "$ROOT/build.sh"
fi

DIST="$ROOT/dist/$TAG"
rm -rf "$DIST"
mkdir -p "$DIST"

# arduino-cli writes the same names every time; rename to the published ones.
copy() {
  local src="$1" dst="$2"
  [ -f "$src" ] || { echo "missing build artifact: $src" >&2; exit 1; }
  cp "$src" "$DIST/$dst"
}

echo "==> assemble artifacts"
copy "$ROOT/out/ats-mini.ino.bin"              "${BASE}_app_0x010000.bin"
copy "$ROOT/build/boot_app0.bin"               "${BASE}_boot_app0_0x00e000.bin"
copy "$ROOT/out/ats-mini.ino.bootloader.bin"   "${BASE}_bootloader_0x000000.bin"
copy "$ROOT/out/ats-mini.ino.partitions.bin"   "${BASE}_partitions_0x008000.bin"
copy "$ROOT/out/ats-mini.ino.merged.bin"       "${BASE}_merged_full8MB_0x000000.bin"

# The merged image is what "救砖" users flash at 0x0: it has to be the whole
# 8 MB chip, with the four pieces at the offsets the partition table declares.
MERGED="$DIST/${BASE}_merged_full8MB_0x000000.bin"
if [ "$(stat -c %s "$MERGED")" -ne 8388608 ]; then
  echo "merged image is not 8 MB, refusing to publish" >&2
  exit 1
fi

echo "==> checksums"
( cd "$DIST" && md5sum *.bin | tee MD5SUMS )

# ---------------------------------------------------------------- tag -------
echo "==> tag $TAG"
if git -C "$ROOT" rev-parse -q --verify "refs/tags/$TAG" > /dev/null; then
  if [ "$FORCE" -eq 0 ]; then
    echo "tag $TAG already exists (use --force to move it)" >&2
    exit 1
  fi
  git -C "$ROOT" tag -d "$TAG" > /dev/null
fi
git -C "$ROOT" tag -a "$TAG" -m "ats-mini-cn $TAG"
git -C "$ROOT" push --force "https://x-access-token:$GITHUB_TOKEN@github.com/$REPO.git" "refs/tags/$TAG"

# ------------------------------------------------------------ release ------
echo "==> release"
EXISTING="$(api GET "$API/repos/$REPO/releases/tags/$TAG" | json "d.get('id','')")"
if [ -n "$EXISTING" ]; then
  if [ "$FORCE" -eq 0 ]; then
    echo "release $TAG already exists (use --force to replace it)" >&2
    exit 1
  fi
  echo "    replacing release $EXISTING"
  api DELETE "$API/repos/$REPO/releases/$EXISTING" > /dev/null
fi

COMMIT="$(git -C "$ROOT" rev-parse --short HEAD)"
DATE="$(date -u '+%Y-%m-%d %H:%M UTC')"
MD5_APP="$(md5sum "$DIST/${BASE}_app_0x010000.bin" | cut -d' ' -f1)"
MD5_MERGED="$(md5sum "$MERGED" | cut -d' ' -f1)"

BODY="$(cat <<EOF
### ats-mini-cn $TAG

基于官方 [esp32-si4732/ats-mini](https://github.com/esp32-si4732/ats-mini) v2.45 的简体中文汉化版。

- 源码提交：\`$COMMIT\`
- 构建时间：$DATE
- 固件版本：\`v2.45\`（\`VER_APP 245\`，屏幕与 About 页显示；迭代号只在标签里）

### 文件与写入地址

| 文件 | 地址 | 用途 |
|---|---|---|
| \`${BASE}_app_0x010000.bin\` | \`0x010000\` | 仅应用程序，**保留**设置/存台/EiBi/开机图，也可网页 OTA |
| \`${BASE}_boot_app0_0x00e000.bin\` | \`0x00e000\` | otadata 初值 |
| \`${BASE}_bootloader_0x000000.bin\` | \`0x000000\` | 二级 bootloader |
| \`${BASE}_partitions_0x008000.bin\` | \`0x008000\` | 分区表 |
| \`${BASE}_merged_full8MB_0x000000.bin\` | \`0x000000\` | 8 MB 整片镜像，**会清空**设置/存台/EiBi/开机图 |

日常升级只刷 \`app\`（或走网页 OTA）；换片、救砖用 \`merged\`。

### 烧写

\`\`\`bash
# 只升级应用，保留所有数据
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x010000 ${BASE}_app_0x010000.bin

# 整片重刷（8 MB）
esptool.py --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash 0x000000 ${BASE}_merged_full8MB_0x000000.bin
\`\`\`

### 校验

\`\`\`
app     $MD5_APP
merged  $MD5_MERGED
\`\`\`

完整清单见附件 \`MD5SUMS\`。
EOF
)"

PAYLOAD="$(
  BASE="$BASE" TAG="$TAG" BODY="$BODY" "$PYTHON" -c '
import json,os
print(json.dumps({"tag_name":os.environ["TAG"],"name":os.environ["TAG"],
                  "body":os.environ["BODY"],"draft":False,"prerelease":False}))
'
)"

RESP="$(api POST "$API/repos/$REPO/releases" -H "Content-Type: application/json" -d "$PAYLOAD")"
RELEASE_ID="$(printf '%s' "$RESP" | json "d.get('id','')")"
UPLOAD="$(printf '%s' "$RESP" | json "d.get('upload_url','').split('{')[0]")"
if [ -z "$RELEASE_ID" ] || [ -z "$UPLOAD" ]; then
  echo "release creation failed:" >&2
  printf '%s\n' "$RESP" >&2
  exit 1
fi

for f in "$DIST"/*.bin "$DIST"/MD5SUMS; do
  name="$(basename "$f")"
  echo "    upload $name"
  code="$(curl -sS -o /tmp/rel_upload.json -w '%{http_code}' -X POST \
    -H "Authorization: token $GITHUB_TOKEN" \
    -H "Content-Type: application/octet-stream" \
    --data-binary "@$f" "$UPLOAD?name=$name")"
  if [ "$code" != "201" ]; then
    echo "upload of $name failed (HTTP $code):" >&2
    cat /tmp/rel_upload.json >&2
    exit 1
  fi
done

echo
echo "==> https://github.com/$REPO/releases/tag/$TAG"
