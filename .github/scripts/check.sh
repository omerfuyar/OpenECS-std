#!/bin/sh
# Formats the plugins' C files with clang-format and checks them with clang-tidy (.clang-format, .clang-tidy), for agents before they commit.
# With no files named, it takes the C files of plugins/ that differ from dev, committed or not.
# It prints only the problems, and fails when clang-tidy finds one.
# It runs in the std/ folder of an OpenECS checkout, after OpenECS's Debug build, whose include/ folder has OpenECS.h and SDL.
set -u
cd "$(dirname "$0")/../.." || exit 2

if [ $# -eq 0 ]; then
    base=$(git merge-base HEAD origin/dev 2> /dev/null || echo HEAD)
    # shellcheck disable=SC2046
    set -- $({
        git diff --name-only "$base"
        git ls-files --others --exclude-standard
    } | grep -E '^plugins/.*\.[ch]$' | sort -u)
fi

files=
for file in "$@"; do
    [ -f "$file" ] && files="$files $file"
done
[ -z "$files" ] && exit 0

# shellcheck disable=SC2086
clang-format -i $files || exit 2

build=../build/Debug/include
if [ ! -f "$build/OpenECS.h" ]; then
    echo "clang-tidy needs OpenECS's Debug build first: in OpenECS, .github/scripts/build.sh D"
    exit 2
fi

# the flags of a plugin's Debug build in shuild.c, with the include folders of every plugin's libraries;
# glslang's build writes one of its headers into .shu/
d=dependencies
flags="-std=c23 -DDEBUG -DSDL_ASSERT_LEVEL=2 -DMBEDTLS_USER_CONFIG_FILE=<mbedtls_user_config.h> -DCURL_STATICLIB
    -isystem $build -isystem $build/stb -isystem $d/stb -isystem $d/nanosvg/src -isystem $d/dr_libs -isystem $d/SDL_net
    -isystem $d/SDL_net/include -isystem $d/cgltf -isystem $d/cglm/include -isystem $d/mbedtls/include -isystem $d/other/mbedtls
    -isystem $d/curl/include -isystem $d/glslang -isystem .shu/Debug/cmake/glslang/include -isystem $d/SPIRV-Cross"

failed=0
for file in $files; do
    case "$file" in
    *.c) ;;
    *) continue ;;
    esac

    # shellcheck disable=SC2086
    if ! clang-tidy --quiet "$file" -- $flags > ../build/tidy.log 2>&1; then
        grep -E "(warning|error):" -A3 ../build/tidy.log | grep -v -E "^[0-9]+ warnings? generated"
        failed=1
    fi
done

exit $failed
