# qemu-arm 8.2.2 with TCG plugins, plus icount.so: counts guest instructions
# between two addresses (tests/perf/realsets.sh). Built for the host's
# architecture: docker build -t dscheevos-qplug:8.2.2 tests/perf/qplug
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      build-essential ninja-build python3-venv libglib2.0-dev pkg-config curl xz-utils flex bison >/dev/null
RUN mkdir -p /q && cd /q && curl -sSfLO https://download.qemu.org/qemu-8.2.2.tar.xz && tar xf qemu-8.2.2.tar.xz && \
    cd qemu-8.2.2 && mkdir b && cd b && \
    ../configure --target-list=arm-linux-user --enable-plugins --disable-docs --disable-tools --disable-werror >/dev/null && \
    ninja qemu-arm >/dev/null && cp qemu-arm /usr/local/bin/qemu-arm-plug && \
    cp ../include/qemu/qemu-plugin.h /usr/local/include/ && cd / && rm -rf /q
COPY icount.c /usr/local/src/icount.c
RUN gcc -shared -fPIC -O2 $(pkg-config --cflags glib-2.0) /usr/local/src/icount.c -o /usr/local/lib/icount.so
