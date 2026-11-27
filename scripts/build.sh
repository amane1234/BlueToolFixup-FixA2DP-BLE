#!/bin/bash
#
# Builds this repository's kexts on macOS, the same way its GitHub CI does.
# The same script is used in IntelBluetoothFirmware and BlueToolFixup-FixA2DP-BLE
# (BrcmPatchRAM): it detects the Xcode project in the repository root.
#
#   scripts/build.sh                 Release build
#   scripts/build.sh -c Debug        Debug build
#   scripts/build.sh -c all          Debug and Release
#   scripts/build.sh --clean         remove build/ first
#   scripts/build.sh --update-deps   update MacKernelSDK and rebuild Lilu.kext
#
# Needs Xcode (xcodebuild) and git. On the first run it clones MacKernelSDK into the
# repository root and builds a Debug Lilu.kext from source next to it (both are
# git-ignored). Environment overrides:
#   LILU_KEXT=/path/Lilu.kext   use this Lilu.kext (a DEBUG release works) instead of building one
#   LILU_REF=<branch|tag|sha>   Lilu revision to build (default: master)
#   MKSDK_REF=<branch|tag|sha>  MacKernelSDK revision (default: master)
#
# The resulting zips are listed at the end (under build/, in a directory named after the configuration).

set -euo pipefail

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

configs=(Release)
clean=0
update=0
while [ $# -gt 0 ]; do
    case "$1" in
        -c|--configuration)
            [ $# -ge 2 ] || usage 1
            case "$2" in
                Debug|Release) configs=("$2") ;;
                all) configs=(Debug Release) ;;
                *) echo "unknown configuration: $2 (Debug, Release or all)" >&2; exit 1 ;;
            esac
            shift ;;
        --clean) clean=1 ;;
        --update-deps) update=1 ;;
        -h|--help) usage 0 ;;
        *) echo "unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

if [ "$(uname -s)" != Darwin ]; then
    echo "error: kexts can only be built on macOS with Xcode" >&2
    exit 1
fi
for tool in git xcodebuild; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool not found (install Xcode and select it with xcode-select)" >&2
        exit 1
    fi
done

if [ -d IntelBluetoothFirmware.xcodeproj ]; then
    project=IntelBluetoothFirmware
    build_args=(-project IntelBluetoothFirmware.xcodeproj -alltargets)
elif [ -d BrcmPatchRAM.xcodeproj ]; then
    project=BrcmPatchRAM
    # Package builds every kext (BlueToolFixup included) and zips them, as the CI does.
    build_args=(-project BrcmPatchRAM.xcodeproj -jobs 1 -target Package)
else
    echo "error: no supported Xcode project in $root" >&2
    exit 1
fi

# The firmware generator imports a Python module from scripts/: keep its bytecode out of the tree.
export PYTHONDONTWRITEBYTECODE=1

run_xcodebuild() {
    if command -v xcpretty >/dev/null 2>&1; then
        xcodebuild "$@" | xcpretty
    else
        xcodebuild -quiet "$@"
    fi
}

checkout() { # dir url ref
    local dir=$1 url=$2 ref=$3
    if [ ! -d "$dir/.git" ]; then
        echo "==> cloning $url ($ref)"
        git clone --quiet "$url" "$dir"
    elif [ "$update" = 1 ]; then
        echo "==> updating $dir"
        git -C "$dir" fetch --quiet origin
    else
        return 0
    fi
    git -C "$dir" checkout --quiet "$ref" 2>/dev/null || git -C "$dir" checkout --quiet "origin/$ref"
    # A branch name follows its remote; a tag or commit is used as is.
    if git -C "$dir" symbolic-ref -q HEAD >/dev/null; then
        git -C "$dir" merge --quiet --ff-only "origin/$ref"
    fi
}

checkout MacKernelSDK https://github.com/acidanthera/MacKernelSDK.git "${MKSDK_REF:-master}"

if [ -n "${LILU_KEXT:-}" ]; then
    if [ ! -f "$LILU_KEXT/Contents/Resources/Headers/kern_api.hpp" ]; then
        echo "error: $LILU_KEXT does not contain the Lilu SDK headers" >&2
        exit 1
    fi
    if [ "$(cd "$LILU_KEXT" && pwd)" != "$root/Lilu.kext" ]; then
        rm -rf Lilu.kext
        cp -R "$LILU_KEXT" Lilu.kext
    fi
elif [ ! -d Lilu.kext ] || [ "$update" = 1 ]; then
    src="$root/build/lilu-src"
    mkdir -p "$root/build"
    checkout "$src" https://github.com/acidanthera/Lilu.git "${LILU_REF:-master}"
    ln -sfn "$root/MacKernelSDK" "$src/MacKernelSDK"
    echo "==> building Lilu.kext (Debug, needed for its SDK)"
    (cd "$src" && run_xcodebuild -configuration Debug -arch x86_64)
    if [ ! -d "$src/build/Debug/Lilu.kext" ]; then
        echo "error: Lilu build did not produce build/Debug/Lilu.kext" >&2
        exit 1
    fi
    rm -rf Lilu.kext
    cp -R "$src/build/Debug/Lilu.kext" Lilu.kext
fi

if [ "$clean" = 1 ]; then
    echo "==> cleaning"
    for config in "${configs[@]}"; do
        run_xcodebuild "${build_args[@]}" -configuration "$config" clean
    done
    # Keep the Lilu source checkout; everything else in build/ is regenerated.
    find build -mindepth 1 -maxdepth 1 ! -name lilu-src -exec rm -rf {} + 2>/dev/null || true
fi

for config in "${configs[@]}"; do
    echo "==> building $project ($config)"
    run_xcodebuild "${build_args[@]}" -configuration "$config"
done

echo "==> done; archives:"
found=0
for config in "${configs[@]}"; do
    while IFS= read -r zip; do
        echo "  ${zip#"$root"/}"
        found=1
    done < <(find "$root/build" -path "$root/build/lilu-src" -prune -o -path "*/$config/*" -name '*.zip' -print | sort)
done
if [ "$found" = 0 ]; then
    echo "error: the build produced no zip archive" >&2
    exit 1
fi
