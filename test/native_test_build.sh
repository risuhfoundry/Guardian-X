#!/usr/bin/env bash
# Fresh native test build for GUARDIAN X.
#
# PlatformIO's `pio test -e native` requires GCC/G++ and does not provide
# mbedTLS for the security tests, so this script compiles the CURRENT sources
# directly with a host compiler (g++/gcc, falling back to MSVC cl.exe) and
# runs both test suites.  Nothing here reuses a stale test executable.
#
# Usage: test/native_test_build.sh
# Dependencies are downloaded once into .pio/native-deps/ (gitignored).
set -euo pipefail

cd "$(dirname "$0")/.."

MBEDTLS_VERSION="mbedtls-2.28.8"
DEPS_DIR=".pio/native-deps"
MBEDTLS_DIR="$DEPS_DIR/$MBEDTLS_VERSION"
MBEDTLS_TARBALL="$DEPS_DIR/v2.28.8.tar.gz"
UNITY_DIR=".pio/libdeps/native/Unity/src"
OBJ_DIR="$DEPS_DIR/obj"
BIN_DIR="$DEPS_DIR/bin"

# md_wrap.c does not exist in mbedTLS 2.28 (it is a 3.x file); the MD
# dispatch used by mbedtls_md_info_from_type lives in md.c itself.
MBEDTLS_C_SOURCES="md.c sha256.c pkcs5.c platform.c platform_util.c"

# --- Fetch dependencies ------------------------------------------------------

if [ ! -d "$MBEDTLS_DIR/library" ]; then
  mkdir -p "$DEPS_DIR"
  if [ ! -f "$MBEDTLS_TARBALL" ]; then
    echo "Downloading mbedTLS v2.28.8 sources..."
    curl -fsSL -o "$MBEDTLS_TARBALL" \
      "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.8.tar.gz"
  fi
  tar -xzf "$MBEDTLS_TARBALL" -C "$DEPS_DIR"
fi

# The host build uses the minimal PBKDF2/SHA-256 configuration checked into
# test/.  In mbedTLS 2.28 the authoritative header is config.h while
# mbedtls_config.h is a compatibility alias, so both are replaced.
cp test/mbedtls_config_minimal.h "$MBEDTLS_DIR/include/mbedtls/config.h"
cp test/mbedtls_config_minimal.h "$MBEDTLS_DIR/include/mbedtls/mbedtls_config.h"

if [ ! -f "$UNITY_DIR/unity.c" ]; then
  echo "error: Unity sources not found at $UNITY_DIR" >&2
  echo "Run any PlatformIO native build once (or install Unity under libdeps)." >&2
  exit 1
fi

mkdir -p "$OBJ_DIR" "$BIN_DIR"

# --- Compiler selection ------------------------------------------------------

if command -v g++ >/dev/null 2>&1 && command -v gcc >/dev/null 2>&1; then
  COMPILER="gcc"
elif [ -f "/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" ]; then
  COMPILER="msvc"
else
  echo "error: no host compiler found (need g++/gcc, or MSVC with vswhere)." >&2
  exit 1
fi

compile_objects() {
  local cppflags="-Iinclude -I\"$UNITY_DIR\" -I\"$MBEDTLS_DIR/include\""
  if [ "$COMPILER" = "gcc" ]; then
    gcc -std=c99 -O2 -I"$MBEDTLS_DIR/include" -c "$UNITY_DIR/unity.c" \
      -o "$OBJ_DIR/unity.o"
    for source in $MBEDTLS_C_SOURCES; do
      gcc -std=c99 -O2 -I"$MBEDTLS_DIR/include" \
        -c "$MBEDTLS_DIR/library/$source" -o "$OBJ_DIR/$source.o"
    done
    g++ -std=c++11 -O2 -Iinclude -I"$UNITY_DIR" -I"$MBEDTLS_DIR/include" \
      -c src/guardian_x/device_core.cpp -o "$OBJ_DIR/device_core.o"
    g++ -std=c++11 -O2 -Iinclude -I"$UNITY_DIR" -I"$MBEDTLS_DIR/include" \
      -c src/guardian_x/security.cpp -o "$OBJ_DIR/security.o"
    g++ -std=c++11 -O2 -Iinclude -I"$UNITY_DIR" -I"$MBEDTLS_DIR/include" \
      -c test/test_device_core/test_main.cpp -o "$OBJ_DIR/test_device_core.o"
    g++ -std=c++11 -O2 -Iinclude -I"$UNITY_DIR" -I"$MBEDTLS_DIR/include" \
      -c test/test_security/test_main.cpp -o "$OBJ_DIR/test_security.o"
    g++ -o "$BIN_DIR/test_device_core" \
      "$OBJ_DIR/test_device_core.o" "$OBJ_DIR/device_core.o" "$OBJ_DIR/unity.o"
    local mbedtls_objects=""
    for source in $MBEDTLS_C_SOURCES; do
      mbedtls_objects="$mbedtls_objects $OBJ_DIR/$source.o"
    done
    g++ -o "$BIN_DIR/test_security" \
      "$OBJ_DIR/test_security.o" "$OBJ_DIR/device_core.o" "$OBJ_DIR/security.o" \
      "$OBJ_DIR/unity.o" $mbedtls_objects
  else
    # MSVC: run cl/link inside the Visual Studio developer environment via a
    # generated batch file (avoids bash quoting problems with vcvars64.bat).
    local vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
    local vsroot
    vsroot="$("$vswhere" -latest -products '*' \
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationPath | head -n1 | tr -d '\r')"
    if [ -z "$vsroot" ]; then
      echo "error: vswhere found no MSVC C++ toolset installation." >&2
      exit 1
    fi
    local vcvars
    vcvars="$(cygpath -w "$vsroot/VC/Auxiliary/Build/vcvars64.bat")"
    [ -f "$vsroot/VC/Auxiliary/Build/vcvars64.bat" ] || {
      echo "error: vcvars64.bat not found under $vsroot" >&2
      exit 1
    }
    local unity_win mbedtls_win obj_win bin_win
    unity_win="$(cygpath -w "$UNITY_DIR")"
    mbedtls_win="$(cygpath -w "$MBEDTLS_DIR")"
    obj_win="$(cygpath -w "$OBJ_DIR")"
    bin_win="$(cygpath -w "$BIN_DIR")"

    local bat="$OBJ_DIR/build_tests.bat"
    {
      echo "@echo off"
      echo "cd /d \"%~dp0..\\..\\..\""
      echo "if errorlevel 1 exit /b 1"
      echo "call \"$vcvars\" >nul"
      echo "if errorlevel 1 exit /b 1"
      echo "cl /nologo /O2 /c \"$unity_win\\unity.c\" /Fo\"$obj_win\\unity.obj\" || exit /b 1"
      for source in $MBEDTLS_C_SOURCES; do
        echo "cl /nologo /O2 /I\"$mbedtls_win\\include\" /c \"$mbedtls_win\\library\\$source\" /Fo\"$obj_win\\$source.obj\" || exit /b 1"
      done
      echo "cl /nologo /O2 /EHsc /std:c++14 /I. /Iinclude /I\"$unity_win\" /I\"$mbedtls_win\\include\" /c src\\guardian_x\\device_core.cpp /Fo\"$obj_win\\device_core.obj\" || exit /b 1"
      echo "cl /nologo /O2 /EHsc /std:c++14 /I. /Iinclude /I\"$unity_win\" /I\"$mbedtls_win\\include\" /c src\\guardian_x\\security.cpp /Fo\"$obj_win\\security.obj\" || exit /b 1"
      echo "cl /nologo /O2 /EHsc /std:c++14 /I. /Iinclude /I\"$unity_win\" /I\"$mbedtls_win\\include\" /c test\\test_device_core\\test_main.cpp /Fo\"$obj_win\\test_device_core.obj\" || exit /b 1"
      echo "cl /nologo /O2 /EHsc /std:c++14 /I. /Iinclude /I\"$unity_win\" /I\"$mbedtls_win\\include\" /c test\\test_security\\test_main.cpp /Fo\"$obj_win\\test_security.obj\" || exit /b 1"
      echo "link /nologo /OUT:\"$bin_win\\test_device_core.exe\" \"$obj_win\\test_device_core.obj\" \"$obj_win\\device_core.obj\" \"$obj_win\\unity.obj\" || exit /b 1"
      local mbedtls_objs=""
      for source in $MBEDTLS_C_SOURCES; do
        mbedtls_objs="$mbedtls_objs \"$obj_win\\$source.obj\""
      done
      echo "link /nologo /OUT:\"$bin_win\\test_security.exe\" \"$obj_win\\test_security.obj\" \"$obj_win\\device_core.obj\" \"$obj_win\\security.obj\" \"$obj_win\\unity.obj\"$mbedtls_objs || exit /b 1"
    } > "$bat"
    cmd //c "$(cygpath -w "$bat")"
  fi
}

compile_objects

echo
echo "===== Running test_device_core ====="
"$BIN_DIR/test_device_core"
echo
echo "===== Running test_security ====="
"$BIN_DIR/test_security"
echo
echo "All native tests passed."
