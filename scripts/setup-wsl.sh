#!/bin/bash
set -e

echo "=== AudioPlayer WSL 构建环境设置 ==="
echo ""

# 检查是否在 WSL 中
if ! grep -qi microsoft /proc/version 2>/dev/null; then
    echo "WARNING: 此脚本专为 WSL 环境设计"
fi

# 1. 安装基础构建工具
echo "[1/7] 安装基础构建工具..."
sudo apt update
sudo apt install -y \
    build-essential cmake git pkg-config nasm \
    libasound2-dev ffmpeg \
    alsa-utils gdb pipx

# 2. 安装 Qt6 6.11.1 via aqtinstall
echo ""
echo "[2/7] 安装 Qt6 6.11.1 (via aqtinstall)..."
pipx ensurepath
export PATH="$HOME/.local/bin:$PATH"

if ! command -v aqt &>/dev/null; then
    pipx install aqtinstall
fi

QT_DIR="$HOME/Qt"
QT_VERSION="6.11.1"
QT_ARCH="linux_gcc_64"
QT_PREFIX="$QT_DIR/$QT_VERSION/$QT_ARCH"

if [ -d "$QT_PREFIX/lib/cmake/Qt6" ]; then
    echo "  Qt6 $QT_VERSION 已安装: $QT_PREFIX"
else
    aqt install-qt linux desktop "$QT_VERSION" "$QT_ARCH" -m qtmultimedia -O "$QT_DIR"
    echo "  Qt6 $QT_VERSION 安装完成: $QT_PREFIX"
fi

# 3. 验证工具链
echo ""
echo "[3/7] 验证工具链..."
cmake --version
echo "Qt6 config: $QT_PREFIX/lib/cmake/Qt6"
pkg-config --cflags --libs alsa

# 4. 项目目录
WSL_PROJECT="/mnt/f/AI/Mimo/AudioPlayer"
if [ -d "$WSL_PROJECT" ]; then
    echo ""
    echo "[4/7] 项目目录: $WSL_PROJECT"
else
    echo ""
    echo "[4/7] 项目目录不存在: $WSL_PROJECT"
    echo "  请确认 Windows 磁盘已挂载到 /mnt/f"
    exit 1
fi

# 5. 构建 FFmpeg 音频核心
echo ""
echo "[5/7] 构建 FFmpeg 音频核心 (8.1.1 slim)..."
FFMPEG_PREFIX="$WSL_PROJECT/build-linux/ffmpeg-audio-core"
if [ -f "$FFMPEG_PREFIX/.build-stamp" ]; then
    echo "  FFmpeg 音频核心已构建: $FFMPEG_PREFIX"
else
    bash "$WSL_PROJECT/scripts/build-ffmpeg-audio-core-linux.sh"
fi

# 6. 创建构建目录
echo ""
echo "[6/7] 创建构建目录..."
BUILD_DIR="$WSL_PROJECT/build-linux"
mkdir -p "$BUILD_DIR"

# 7. 构建 AudioPlayer
echo ""
echo "[7/7] 开始构建 AudioPlayer..."
cd "$BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DAUDIOPLAYER_FFMPEG_AUDIO_CORE_ROOT="$WSL_PROJECT/build-linux/ffmpeg-audio-core"
cmake --build . --parallel $(nproc)

echo ""
echo "=== 构建完成 ==="
echo "可执行文件: $BUILD_DIR/AudioPlayer"
echo "FFmpeg: $FFMPEG_PREFIX/bin/ffmpeg"
echo "FFprobe: $FFMPEG_PREFIX/bin/ffprobe"
echo ""
echo "运行:"
echo "  export AUDIOPLAYER_FFMPEG_PATH=$FFMPEG_PREFIX/bin/ffmpeg"
echo "  export AUDIOPLAYER_FFPROBE_PATH=$FFMPEG_PREFIX/bin/ffprobe"
echo "  cd $BUILD_DIR && ./AudioPlayer"
echo ""
echo "注意: WSL 中音频通过 WSLg 虚拟设备输出，hw: 独占模式无法测试"
