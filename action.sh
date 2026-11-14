#!/system/bin/sh
MODDIR="${0%/*}"
[ "${MODDIR}" = "$0" ] && MODDIR="/data/adb/modules/anmian_screen_off"
cd "${MODDIR}" 2>/dev/null || exit 1

echo "=== 安眠[息屏管理] ==="
pid="$(pidof App_screen_off 2>/dev/null)"
if [ -n "${pid}" ]; then
	echo "状态：运行中（PID ${pid}）"
else
	echo "状态：未运行"
fi

echo ""
echo "--- 配置 ---"
grep -v '^[[:space:]]*#' "${MODDIR}/配置.prop" 2>/dev/null | grep -v '^[[:space:]]*$'

echo ""
echo "--- 最近日志 ---"
tail -n 15 "${MODDIR}/运行日志.log" 2>/dev/null || echo "暂无日志"

exit 0
