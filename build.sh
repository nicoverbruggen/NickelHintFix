#!/usr/bin/env bash
set -euo pipefail

image="${NICKELTC_IMAGE:-ghcr.io/pgaskin/nickeltc:1.0}"
workdir="${PWD}"
scratch="${workdir}/tmp/build"

export COPYFILE_DISABLE=1

if [ "$#" -eq 0 ]; then
    set -- clean all strip koboroot
fi

mkdir -p "${scratch}"

tar \
    --no-mac-metadata \
    --no-xattrs \
    --no-acls \
    --no-fflags \
    -C "${workdir}" \
    --exclude=.git \
    --exclude=.DS_Store \
    --exclude=tmp \
    --exclude=KoboRoot.tgz \
    --exclude='*.o' \
    --exclude='*.moc' \
    --exclude=nhplugin.json \
    --exclude=src/libnickelhintfix.so \
    -czf "${scratch}/source.tgz" .

podman run --rm -i \
    --entrypoint sh \
    "${image}" \
    -lc '
        set -eu
        mkdir -p /work
        tar -C /work -xzf -
        cd /work
        make "$@" \
            CROSS_COMPILE=/tc/arm-nickel-linux-gnueabihf/bin/arm-nickel-linux-gnueabihf- \
            MOC=/tc/arm-nickel-linux-gnueabihf/arm-nickel-linux-gnueabihf/sysroot/usr/bin/moc \
            RCC=/tc/arm-nickel-linux-gnueabihf/arm-nickel-linux-gnueabihf/sysroot/usr/bin/rcc >&2
        if [ -f KoboRoot.tgz ] && [ -f src/libnickelhintfix.so ]; then
            tar -czf - KoboRoot.tgz src/libnickelhintfix.so
        fi
    ' sh "$@" < "${scratch}/source.tgz" > "${scratch}/artifacts.tgz"

if [ -s "${scratch}/artifacts.tgz" ]; then
    tar -xzf "${scratch}/artifacts.tgz" -C "${workdir}"
fi
