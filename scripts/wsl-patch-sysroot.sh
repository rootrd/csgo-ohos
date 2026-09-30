#!/usr/bin/env bash
# Patch OHOS sysroot kernel headers for musl coexistence (WSL copy only).
# linux/socket.h (bionic-generated) unconditionally defines sockaddr_storage;
# musl's sys/socket.h already provides it -> redefinition in any TU including both.
set -ex
F=/root/ohos-native/sysroot/usr/include/linux/socket.h
python3 - "$F" <<'PY'
import io, sys
p = sys.argv[1]
s = io.open(p, encoding='utf-8').read()
if '_SYS_SOCKET_H' in s:
    print('already patched'); raise SystemExit
old = '''#define _K_SS_MAXSIZE 128
typedef unsigned short __kernel_sa_family_t;
struct sockaddr_storage {
  union {
    struct {
      __kernel_sa_family_t ss_family;
      char __data[_K_SS_MAXSIZE - sizeof(unsigned short)];
    };
    void * __align;
  };
};'''
new = '''#define _K_SS_MAXSIZE 128
typedef unsigned short __kernel_sa_family_t;
#ifndef _SYS_SOCKET_H /* ZCode patch: musl sys/socket.h already defines sockaddr_storage */
struct sockaddr_storage {
  union {
    struct {
      __kernel_sa_family_t ss_family;
      char __data[_K_SS_MAXSIZE - sizeof(unsigned short)];
    };
    void * __align;
  };
};
#endif'''
assert old in s, 'pattern not found'
io.open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new))
print('patched', p)
PY
echo SYSROOT_PATCH_DONE
