#!/bin/bash
NM=/root/ohos-native/llvm/bin/llvm-nm
L=/mnt/e/csgo/hap/entry/libs/arm64-v8a
echo "=== 每个库引用的 gio 家族符号（g_app_info/g_bus/g_dbus/g_subprocess/g_settings/g_file_monitor/g_io_/g_content_/g_unix_mount）==="
for f in "$L"/*.so "$L"/*.so.*; do
  [ -f "$f" ] || continue
  hits=$($NM -D --undefined-only "$f" 2>/dev/null | grep -oE "g_(app_info|bus|dbus|subprocess|settings|file_monitor|io_[a-z_]*|content_type|unix_mount|vfs|volume|notification)[a-z_]*" | sort -u | head -6)
  [ -n "$hits" ] && echo "--- $(basename $f)" && echo "$hits"
done
echo
echo "=== pango / pangoft2 / text client 的全部 gio 相关未定义符号 ==="
for f in libpango-1.0.so.0.5400.0 libpangoft2-1.0.so.0.5400.0 libpanorama_text_pango_client.so; do
  [ -f "$L/$f" ] || continue
  echo "--- $f"
  $NM -D --undefined-only "$L/$f" 2>/dev/null | grep -E " U g_" | grep -vE "g_(file_|object|type_|value_|param_|signal_|error_|quark|free|malloc|new|slice|str|mem|list|slist|hash|array|ptr_array|byte_array|string|utf8|unichar|ascii|date|time|timer|async_queue|thread|mutex|cond|atomic|gettext|snprintf|sprintf|printf|vasprintf|strdup|strndup|strcmp|strncmp|parse|mkdir|unlink|rename|open|creat|remove|stat|lstat|access|chmod|chown|rmdir|symlink|readlink|utime|pattern|shell|spawn|log|assert|test|idle|timeout|source|main_|child_watch|pid|option|key_file|dataset|rand|bookmark|mapped_file|checksum|convert|iconv|base64|environ|setenv|unsetenv|hostname|user_|dir_|filename|build_path|canonicalize|path_|locale|language|reload|variant|regex|sequence|queue|tree|node|once|rw_lock|rec_mutex|private|cond|thread_pool|qsort|sort|steal|memset|mapped|URI|uri_|uuid|file_error|file_set|file_get|mkstemp|tmp|stdio|fdwalk|closefrom|unix_signal|unix_fd|unix_open_pipe|unix_set_fd|unix_get_passwd|pid|bit_|trash|mime|version|threads|nproc|monotonic|real_time|prgname|progname|application_name|top" > /dev/null; done
echo "(空=没有 gio 家族符号)"
echo
echo "=== 全库扫描：谁引用 g_io_channel / g_file_monitor / g_settings / g_bus / g_app_info ==="
for f in "$L"/*.so "$L"/*.so.*; do
  [ -f "$f" ] || continue
  n=$($NM -D --undefined-only "$f" 2>/dev/null | grep -cE "g_(io_channel|file_monitor|settings|bus_get|app_info|subprocess|dbus_)")
  [ "${n:-0}" != "0" ] && echo "$(basename $f): $n"
done
