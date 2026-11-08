#!/usr/bin/env bash
# DevDesk macOS 打包脚本：构建 → DevDesk.app → 补拷 assets → create-dmg 出 DMG
# 用法：bash scripts/package-macos.sh
#
# 环境变量：
#   DEVDESK_BUILD_DIR  构建目录，默认 build/。可给绝对路径，也可给相对仓库根的路径（脚本会
#                      绝对化后透传给 platform/macos/build-app.sh）。指到 build-release 这类
#                      另一棵树上打包，才能保住开发用的 Debug build/ 不被重配成 Release。
#   DEVDESK_JOBS       并发编译单元数，默认 3。
#
# DMG 只能在 macOS 上构建，非 Darwin 直接报错退出（Windows 打包走 scripts/package-windows.sh）。
# 图标（DevDesk.icns / 卷图标）与 Finder 真机表现需在 Mac 上另做。
set -e

# 切到 DevDesk/（脚本在 DevDesk/scripts/ 下）
cd "$(dirname "$0")/.."
PROJECT_DIR="$(pwd)"

# 构建目录：相对路径按仓库根解析 → 后面全部按绝对路径用，传给 build-app.sh 也无歧义
BUILD_DIR="${DEVDESK_BUILD_DIR:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
  /*) : ;;
  *) BUILD_DIR="$PROJECT_DIR/$BUILD_DIR" ;;
esac

case "$(uname -s)" in
  Darwin) : ;;
  *)
    echo "❌ 本脚本只能在 macOS 上运行（DMG 只能在 Mac 上构建，见 spec §4.4）" >&2
    echo "   Windows 打包请用：bash scripts/package-windows.sh" >&2
    exit 1 ;;
esac

# 依赖检查放在构建前：缺 create-dmg 时不必先白跑一遍 Release 构建
if ! command -v create-dmg >/dev/null 2>&1; then
  echo "❌ 找不到 create-dmg（安装：brew install create-dmg）" >&2
  exit 1
fi

# -D 会覆盖已 configure 的树，所以打包必须显式写 -DCMAKE_BUILD_TYPE=Release —— 直接对着开发用的
# Debug build/ 跑会把整棵树重配成 Release 并触发全量重编；要保住 Debug 树就用 DEVDESK_BUILD_DIR
# 指到另一棵树（如 build-release）。
echo "── configure（Release，构建目录 ${BUILD_DIR}）──"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
echo "── 构建 ──"
cmake --build "$BUILD_DIR" --parallel "${DEVDESK_JOBS:-3}" 2>&1 | tail -5

# build-app.sh 会先 rm -rf 再重建 DevDesk.app（.app 内不会残留上次的文件）。
# DEVDESK_SKIP_LSREGISTER=1 必须置上：build-app.sh 里的 lsregister -f 会写用户的 LaunchServices 库，
# 打包流程不该留下这种副作用。
DEVDESK_BUILD_DIR="$BUILD_DIR" DEVDESK_SKIP_LSREGISTER=1 bash platform/macos/build-app.sh
APP_DIR="$BUILD_DIR/DevDesk.app"
MACOS_DIR="$APP_DIR/Contents/MacOS"
if [ ! -x "$MACOS_DIR/DevDesk" ]; then
  echo "❌ .app 内没有可执行文件：$MACOS_DIR/DevDesk（build-app.sh 没跑成功？）" >&2
  exit 1
fi

# 补拷 assets 进 .app，必须与 exe 同级（Contents/MacOS/assets）：EUI-NEO 按「exe 旁」找资源，
# 放进 Contents/Resources/assets 不生效。来源是构建树的 assets/（EUI-NEO 字体/音乐/shaders/icon）。
rm -rf "$MACOS_DIR/assets"
cmake -E copy_directory "$BUILD_DIR/assets" "$MACOS_DIR/assets"
# 防残留断言：cmake -E copy_directory 只增不删，所以上面必须先 rm -rf。与 package-windows.sh 同款。
if [ -z "$(ls -A "$MACOS_DIR/assets/shaders/shadertoy" 2>/dev/null)" ]; then
  echo "❌ 断言失败：$MACOS_DIR/assets/shaders/shadertoy 为空" >&2
  exit 1
fi
# 随包字体就位断言（need:132 字体统一线）：6 文件（双字重子集 + JetBrains Mono +
# 两份 OFL + 修改声明）必须随包；来源=构建树 assets（CMake POST_BUILD 逐文件拷），
# 经上方整树 copy 进 .app，缺任一 = 打包链断了
for fontfile in DevDeskSansSC-Regular.otf DevDeskSansSC-Bold.otf JetBrainsMono-Regular.ttf \
                OFL-NotoSansCJK.txt OFL-JetBrainsMono.txt FONT-MODIFICATIONS.txt; do
  if [ ! -f "$MACOS_DIR/assets/$fontfile" ]; then
    echo "❌ 断言失败：随包字体/许可缺失：$MACOS_DIR/assets/$fontfile" >&2
    exit 1
  fi
done
echo "✅ assets 就位（Contents/MacOS/assets）"

# corta-exec 随包（need:122）：Spawn.swift 按「宿主二进制同级/Contents/MacOS」寻址（clue #23）
if [ -f "$BUILD_DIR/gen/corta/corta-exec" ]; then
  cp "$BUILD_DIR/gen/corta/corta-exec" "$MACOS_DIR/corta-exec"
  echo "✅ corta-exec 就位（Contents/MacOS/corta-exec）"
else
  echo "❌ 缺 $BUILD_DIR/gen/corta/corta-exec（先跑 build-test-run.sh 构建）" >&2
  exit 1
fi

# llama-server 随包（bug #225）：引擎 pkg 定位序 = exe 旁 assets/llama/bin
# （llama_host.cpp locateLlamaServerBetween 序 2，位于 dataroot/brew/PATH 之前 → 包内优先，
# 开箱确定性：用户机不装 brew llama 也能跑；env 覆盖序仍最前，开发机行为不变）。
# brew 版 llama-server 的依赖 = @rpath（解到 llama.cpp lib）+ 绝对路径（ggml/openssl@4/libomp），
# 裸拷到没装 Homebrew 依赖的用户机 dyld 必炸：把依赖闭包整体拷进同一目录，并把全部非系统
# install name 重写为 @loader_path/<basename>，包内自洽；随后 sign-macos-app.sh 会逐个预签
# Contents/MacOS 下常规文件（install_name_tool 使原签名失效，必须重签，由签名脚本兜住）。
# 打包机没装 llama-server → 跳过随包（不阻断打包），装机运行期回落既有 brew/PATH 定位序。
if command -v llama-server >/dev/null 2>&1; then
  LLAMA_DIR="$MACOS_DIR/assets/llama/bin"
  mkdir -p "$LLAMA_DIR"
  # fixpoint 闭包：队列存源侧路径；每个文件 cp -L 拷入（解 Homebrew symlink），非系统依赖
  # 逐条解析（@rpath 用源文件自己的 LC_RPATH）→ 补拷 → 重写为 @loader_path/<basename>
  llama_q=("$(realpath "$(command -v llama-server)")")
  while [ "${#llama_q[@]}" -gt 0 ]; do
    llama_src="${llama_q[0]}"; llama_q=("${llama_q[@]:1}")
    llama_dst="$LLAMA_DIR/$(basename "$llama_src")"
    # 同一 dylib 可能经 Cellar/opt 两条路径重复入队；且 brew 源文件 0444，重复 cp 覆盖必炸
    if [ -f "$llama_dst" ]; then continue; fi
    cp -L "$llama_src" "$llama_dst"
    case "$llama_src" in
      *.dylib) install_name_tool -id "@loader_path/$(basename "$llama_src")" "$llama_dst" ;;
    esac
    while IFS= read -r dep; do
      case "$dep" in /usr/lib/*|/System/*) continue ;; esac
      llama_res=""
      case "$dep" in
        @rpath/*)
          llama_tail="${dep#@rpath}"
          while IFS= read -r rp; do
            case "$rp" in
              @loader_path/*) llama_cand="${llama_src%/*}/${rp#@loader_path/}${llama_tail}" ;;
              /*) llama_cand="${rp}${llama_tail}" ;;
              *) continue ;;
            esac
            if [ -e "$llama_cand" ]; then llama_res="$llama_cand"; break; fi
          done < <(otool -l "$llama_src" | awk '/LC_RPATH/{getline; getline; print $2}')
          ;;
        @loader_path/*) llama_res="${llama_src%/*}/${dep#@loader_path/}" ;;
        @executable_path/*) continue ;;
        /*) llama_res="$dep" ;;
        *) continue ;;
      esac
      if [ -z "$llama_res" ] || [ ! -e "$llama_res" ]; then
        echo "❌ llama 依赖闭包解析失败：$llama_src → $dep" >&2
        exit 1
      fi
      llama_base="$(basename "$llama_res")"
      # 自引用（dylib 的 id 行）或已拷过的依赖不再入队
      if [ ! -f "$LLAMA_DIR/$llama_base" ]; then
        llama_q+=("$llama_res")
      fi
      install_name_tool -change "$dep" "@loader_path/$llama_base" "$llama_dst"
    done < <(otool -L "$llama_src" | tail -n +2 | awk '{print $1}')
  done
  # 防残留断言：闭包内任一文件残留 @rpath 依赖或 Homebrew 绝对路径 = 用户机 dyld 会炸
  if otool -L "$LLAMA_DIR/llama-server" "$LLAMA_DIR"/*.dylib | grep -E "@rpath/|/opt/homebrew" >/dev/null; then
    echo "❌ 断言失败：包内 llama 依赖仍有 @rpath/Homebrew 绝对路径残留" >&2
    exit 1
  fi
  echo "✅ llama-server 随包就位（Contents/MacOS/assets/llama/bin，$(ls "$LLAMA_DIR" | wc -l | tr -d ' ') 个文件）"
else
  echo "⚠️ 打包机未装 llama-server，跳过随包（装机后走 brew/PATH 定位序）"
fi

# 主二进制非系统依赖随包（bug #239，发布阻断）：mac 侧 OpenSSL 走 Homebrew 动态库
# （与 Windows 的 DEVDESK_STATIC 静态链不同），未收编时主二进制硬链
# /opt/homebrew/opt/openssl@3/lib/libssl.3.dylib —— 用户机没装 Homebrew openssl@3
# 时 dyld 加载失败、.app 直接启不来。llama 段的闭包处理只覆盖 llama 自己的依赖树，
# 管不到主二进制，故单开本段。
#
# 实测闭包：主二进制非系统依赖 = libssl.3 + libcrypto.3；libssl 又依赖 libcrypto
# （写成 **Cellar 版本化路径** /opt/homebrew/Cellar/openssl@3/3.5.9/lib/...，与 opt/
# 软链是两个不同字符串）；libcrypto 无再下游。主二进制无 LC_RPATH。
# ⚠️ 必须递归处理拷入的 dylib 自身依赖——只改主二进制的直接依赖会漏掉 libssl 里
# 那条 Cellar 路径（实测踩过：主二进制已全 @loader_path，libssl 仍指 Cellar）。
# 顺序：必须在 sign-macos-app.sh 之前（install_name_tool 让原签名失效，签名脚本兜改）。
MAIN_BIN="$MACOS_DIR/DevDesk"
if [ -f "$MAIN_BIN" ]; then
  # 以主二进制为根做小 fixpoint：队列存「待扫描的源路径」，拷入后扫它自己的非系统依赖
  main_q=("$MAIN_BIN")
  while [ "${#main_q[@]}" -gt 0 ]; do
    main_src="${main_q[0]}"; main_q=("${main_q[@]:1}")
    while IFS= read -r main_dep; do
      case "$main_dep" in /usr/lib/*|/System/*|@loader_path/*|@rpath/*|@executable_path/*) continue ;; esac
      main_base="$(basename "$main_dep")"
      main_dst="$MACOS_DIR/$main_base"
      # cp -L 解 Homebrew 软链拿实体；同一 dylib 经 Cellar/opt 两条路径重复入队时跳过重复拷贝
      if [ ! -f "$main_dst" ]; then
        cp -L "$main_dep" "$main_dst"
        install_name_tool -id "@loader_path/$main_base" "$main_dst"
        main_q+=("$main_dst")  # 递归扫描它自己的依赖
      fi
      install_name_tool -change "$main_dep" "@loader_path/$main_base" "$main_src"
    done < <(otool -L "$main_src" | tail -n +2 | awk '{print $1}')
  done
  unset main_q main_src main_dep main_base main_dst
  # 防残留断言（与 llama 段同款）：bundle 内主二进制/dylib 仍留 Homebrew 绝对路径 = 用户机 dyld 会炸
  if otool -L "$MAIN_BIN" "$MACOS_DIR"/*.dylib 2>/dev/null | grep -E "/opt/homebrew|/usr/local/opt" >/dev/null; then
    echo "❌ 断言失败：主二进制或随包 dylib 仍有 Homebrew 绝对路径残留（bug239 回归）" >&2
    otool -L "$MAIN_BIN" "$MACOS_DIR"/*.dylib 2>/dev/null | grep -E "/opt/homebrew|/usr/local/opt" >&2
    exit 1
  fi
  echo "✅ 主二进制非系统依赖已随包收编（$(ls "$MACOS_DIR"/*.dylib 2>/dev/null | wc -l | tr -d ' ') 个 dylib 于 Contents/MacOS）"
else
  echo "❌ 缺主二进制 $MAIN_BIN" >&2
  exit 1
fi

# 签名：build-app.sh 组装时已签过，但上面 rm -rf + copy_directory 补拷 assets 让封条失效了。
# 必须在所有 bundle 写操作之后重签，签名才是最后一步（见 scripts/sign-macos-app.sh 头注释）。
bash scripts/sign-macos-app.sh "$APP_DIR"

# 版本号单一源 = 顶层 CMakeLists.txt 的 project(DevDesk VERSION ...)，configure 后落到
# CMakeCache.txt 的 CMAKE_PROJECT_VERSION。不要在别处再写一份版本号。
APP_VERSION="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Za-z]*=\(.*\)$/\1/p' "$BUILD_DIR/CMakeCache.txt" | head -1 | tr -d '\r')"
if [ -z "$APP_VERSION" ]; then
  echo "❌ 无法从 $BUILD_DIR/CMakeCache.txt 读取 CMAKE_PROJECT_VERSION" >&2
  exit 1
fi
echo "── 版本: $APP_VERSION ──"

# 暂存目录 = create-dmg 的 <SourceFolder>（App + 去隔离脚本 + 安装说明），放
# installer/macos/out/（已 gitignore，与 DMG 输出同处），不跟随 DEVDESK_BUILD_DIR：暂存内容全部
# 来自已组装好的 $APP_DIR 与源码树的 installer/macos/，对构建树没有语义依赖；放进构建树等于往
# 可能正被别的进程使用的 Debug build/ 里塞一份 Release .app 拷贝。
STAGE="installer/macos/out/dmg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP_DIR" "$STAGE/DevDesk.app"
# 暂存副本再签一次：不依赖 cp -R 是否完整保留签名元数据，确保进 DMG 的那份就是签名有效的
# 字节。DMG 里不能再出现「构建树里签过、拷进来却验不过」的副本。
bash scripts/sign-macos-app.sh "$STAGE/DevDesk.app"
cp "installer/macos/首次运行（去隔离）.command" "$STAGE/"
cp "installer/macos/安装说明.txt" "$STAGE/"
# 本仓不保留可执行位（git ls-files -s scripts/ 全 100644）→ 暂存时补 chmod 并断言，
# 否则 DMG 里双击 .command 无效
chmod 755 "$STAGE/首次运行（去隔离）.command"
if [ ! -x "$STAGE/首次运行（去隔离）.command" ]; then
  echo "❌ 去隔离脚本缺少可执行位" >&2
  exit 1
fi

# create-dmg 的位置参数必须补全 <OutputName.dmg> <SourceFolder>；每个 flag 直接就是窗口几何，
# 漏掉一个不报错，只会给出一个悄悄错位的窗口。
mkdir -p installer/macos/out
DMG="installer/macos/out/DevDesk-$APP_VERSION.dmg"
# --overwrite 必加：否则第二次运行会失败
# 每个进 DMG 的条目都必须给 --icon 坐标：没坐标的项会被 Finder 甩出可视区，用户看不到会以为文件缺失
create-dmg \
  --overwrite \
  --volname "DevDesk" \
  --window-pos 200 120 \
  --window-size 700 440 \
  --icon-size 120 \
  --icon "DevDesk.app" 190 210 \
  --icon "首次运行（去隔离）.command" 110 68 \
  --icon "安装说明.txt" 590 68 \
  --app-drop-link 510 210 \
  --hide-extension "DevDesk.app" \
  --background "installer/macos/background.png" \
  --no-internet-enable \
  "$DMG" \
  "$STAGE"

if [ ! -f "$DMG" ]; then
  echo "❌ 未按预期产出 $DMG" >&2
  exit 1
fi
echo "✅ DMG：$PROJECT_DIR/${DMG}（$(du -h "$DMG" | cut -f1)）"
