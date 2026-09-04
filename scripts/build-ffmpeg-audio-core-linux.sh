#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

FFMPEG_VERSION="${FFMPEG_VERSION:-9.0.1}"
SOURCE_DIR="${SOURCE_DIR:-$REPO_ROOT/build-linux/ffmpeg-src}"
PREFIX_DIR="${PREFIX_DIR:-$REPO_ROOT/build-linux/ffmpeg-audio-core}"
JOBS="${JOBS:-$(nproc)}"
PROFILE="${PROFILE:-runtime-with-ffprobe}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"
DISABLE_X86ASM="${DISABLE_X86ASM:-}"

PROTOCOLS="file,pipe"
DEMUXERS="aac,ac3,eac3,flac,matroska,mov,mp3,truehd,wav"
MUXERS="matroska,null,pcm_f32le,pcm_s16le,pcm_s32le,pcm_u8"
DECODERS="aac,ac3,alac,eac3,flac,mp3,mp3float,pcm_f32le,pcm_s16le,pcm_s24le,pcm_s32le,pcm_u8,truehd"
ENCODERS="pcm_f32le,pcm_s16le,pcm_s32le,pcm_u8"
FILTERS="aformat,aresample,channelmap,pan"
PARSERS="aac,ac3,flac,mlp,mpegaudio"

die() { echo "ERROR: $*" >&2; exit 1; }

download_source() {
    local url="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
    local tarball="$REPO_ROOT/build-linux/ffmpeg-${FFMPEG_VERSION}.tar.xz"

    if [[ -f "$SOURCE_DIR/configure" ]]; then
        local current_version
        current_version=$(head -1 "$SOURCE_DIR/RELEASE" 2>/dev/null || echo "unknown")
        if [[ "$current_version" == "$FFMPEG_VERSION" ]]; then
            echo "[ffmpeg] Source $FFMPEG_VERSION already present at $SOURCE_DIR"
            return 0
        fi
        echo "[ffmpeg] Version mismatch: source=$current_version required=$FFMPEG_VERSION, re-downloading"
        rm -rf "$SOURCE_DIR"
    fi

    echo "[ffmpeg] Downloading FFmpeg $FFMPEG_VERSION..."
    mkdir -p "$(dirname "$tarball")"
    curl -fSL -o "$tarball" "$url"
    mkdir -p "$REPO_ROOT/build-linux"
    tar xf "$tarball" -C "$REPO_ROOT/build-linux"
    mv "$REPO_ROOT/build-linux/ffmpeg-${FFMPEG_VERSION}" "$SOURCE_DIR"
    rm -f "$tarball"
    echo "[ffmpeg] Source extracted to $SOURCE_DIR"
}

check_stamp() {
    local stamp_file="$PREFIX_DIR/.build-stamp"
    if [[ "$FORCE_REBUILD" == "1" ]]; then
        return 1
    fi
    if [[ ! -f "$stamp_file" ]]; then
        return 1
    fi

    local stamp_version stamp_configure_sha
    stamp_version=$(grep "^version=" "$stamp_file" | cut -d= -f2)
    stamp_configure_sha=$(grep "^configureSha256=" "$stamp_file" | cut -d= -f2)

    local current_sha
    current_sha=$(sha256sum "$SOURCE_DIR/configure" | cut -d' ' -f1)

    if [[ "$stamp_version" == "$FFMPEG_VERSION" && "$stamp_configure_sha" == "$current_sha" ]]; then
        echo "[ffmpeg] Build stamp matches ($FFMPEG_VERSION), skipping rebuild"
        echo "[ffmpeg] runtimeVersion=$FFMPEG_VERSION"
        return 0
    fi

    return 1
}

build_ffmpeg() {
    if check_stamp; then
        return 0
    fi

    echo "[ffmpeg] Configuring FFmpeg $FFMPEG_VERSION (profile=$PROFILE)..."
    mkdir -p "$PREFIX_DIR"

    local configure_args=(
        --prefix="$PREFIX_DIR"
        --disable-autodetect
        --disable-avdevice
        --disable-debug
        --disable-doc
        --disable-everything
        --disable-network
        --disable-stripping
        --disable-shared
        --enable-avcodec
        --enable-avfilter
        --enable-avformat
        --enable-ffmpeg
        --enable-swresample
        --enable-protocol="$PROTOCOLS"
        --enable-demuxer="$DEMUXERS"
        --enable-muxer="$MUXERS"
        --enable-decoder="$DECODERS"
        --enable-encoder="$ENCODERS"
        --enable-filter="$FILTERS"
        --enable-parser="$PARSERS"
    )

    if [[ "$PROFILE" == "runtime-with-ffprobe" ]]; then
        configure_args+=(--enable-ffprobe)
    else
        configure_args+=(--disable-ffprobe)
    fi

    if ! command -v nasm &>/dev/null; then
        die "nasm not found. Install with: sudo apt install -y nasm"
    fi

    pushd "$SOURCE_DIR" > /dev/null
    echo "[ffmpeg] ./configure ${configure_args[*]}"
    ./configure "${configure_args[@]}"

    echo "[ffmpeg] Building with $JOBS jobs..."
    make -j"$JOBS"
    make install
    popd > /dev/null

    local configure_sha
    configure_sha=$(sha256sum "$SOURCE_DIR/configure" | cut -d' ' -f1)
    cat > "$PREFIX_DIR/.build-stamp" <<EOF
version=$FFMPEG_VERSION
configureSha256=$configure_sha
profile=$PROFILE
builtAt=$(date -u +%Y-%m-%dT%H:%M:%SZ)
EOF

    echo "[ffmpeg] Build complete: $PREFIX_DIR"
}

verify_build() {
    local ffmpeg_bin="$PREFIX_DIR/bin/ffmpeg"
    local ffprobe_bin="$PREFIX_DIR/bin/ffprobe"

    if [[ ! -x "$ffmpeg_bin" ]]; then
        die "ffmpeg binary not found at $ffmpeg_bin"
    fi

    echo "[ffmpeg] ffmpeg: $($ffmpeg_bin -version 2>&1 | head -1)"

    if [[ "$PROFILE" == "runtime-with-ffprobe" ]]; then
        if [[ ! -x "$ffprobe_bin" ]]; then
            die "ffprobe binary not found at $ffprobe_bin"
        fi
        echo "[ffmpeg] ffprobe: $($ffprobe_bin -version 2>&1 | head -1)"
    fi
}

main() {
    echo "=== FFmpeg Audio-Core Build (Linux) ==="
    echo "  Version: $FFMPEG_VERSION"
    echo "  Profile: $PROFILE"
    echo "  Source:  $SOURCE_DIR"
    echo "  Prefix:  $PREFIX_DIR"
    echo "  Jobs:    $JOBS"
    echo ""

    download_source
    build_ffmpeg
    verify_build

    echo ""
    echo "=== Done ==="
    echo "ffmpeg: $PREFIX_DIR/bin/ffmpeg"
    echo "ffprobe: $PREFIX_DIR/bin/ffprobe"
    echo ""
    echo "Usage:"
    echo "  export AUDIOPLAYER_FFMPEG_PATH=$PREFIX_DIR/bin/ffmpeg"
    echo "  export AUDIOPLAYER_FFPROBE_PATH=$PREFIX_DIR/bin/ffprobe"
}

main "$@"
