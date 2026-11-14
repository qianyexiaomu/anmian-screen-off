#!/system/bin/sh

MOD="/data/adb/modules/anmian_screen_off"
BIN="${MOD}/App_screen_off"
TS="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="/sdcard/Download"
[ -d "${OUT_DIR}" ] || OUT_DIR="/data/local/tmp"
WORK="${OUT_DIR}/anmian_diag_${TS}"
REPORT="${OUT_DIR}/anmian_diag_${TS}.txt"

mkdir -p "${WORK}" 2>/dev/null

head_line() {
	echo "" >>"${REPORT}"
	echo "===== $1 =====" >>"${REPORT}"
}

grab() {
	# $1 标题, $2 命令
	head_line "$1"
	sh -c "$2" >>"${REPORT}" 2>&1
}

raw() {
	# $1 文件名, $2 命令
	sh -c "$2" >"${WORK}/$1" 2>&1
}

say() {
	echo "$1"
	echo "$1" >>"${REPORT}"
}

echo "安眠[息屏管理] 诊断包" >"${REPORT}"
echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')" >>"${REPORT}"
echo "本报告只收集环境信息，不会改动系统设置" >>"${REPORT}"

head_line "模块状态"
{
	echo "模块目录: $([ -d "${MOD}" ] && echo 存在 || echo 不存在)"
	[ -f "${MOD}/module.prop" ] && sed -n 's/^/  /p' "${MOD}/module.prop"
	echo -n "进程: "
	pidof App_screen_off 2>/dev/null || echo "未运行"
} >>"${REPORT}" 2>&1

grab "设备信息" '
echo "品牌: $(getprop ro.product.brand)"
echo "型号: $(getprop ro.product.model)"
echo "厂商: $(getprop ro.product.manufacturer)"
echo "设备代号: $(getprop ro.product.device)"
echo "Android: $(getprop ro.build.version.release) (SDK $(getprop ro.build.version.sdk))"
echo "ABI: $(getprop ro.product.cpu.abi)"
echo "系统版本: $(getprop ro.build.display.id)"
echo "指纹: $(getprop ro.build.fingerprint)"
echo "内核: $(uname -a)"
echo "SELinux: $(getenforce 2>/dev/null)"
echo "运行时间: $(uptime)"
'

grab "Root 环境" '
for c in magisk ksud apd; do
  if command -v $c >/dev/null 2>&1; then
    case $c in
      magisk) echo "Magisk: $(magisk -V 2>/dev/null)" ;;
      ksud)   echo "KernelSU: $(ksud -V 2>/dev/null)" ;;
      apd)    echo "APatch: $(apd -V 2>/dev/null)" ;;
    esac
  fi
done
ls -1 /data/adb 2>/dev/null | sed "s/^/  \/data\/adb\//"
'

grab "屏幕" '
echo "分辨率: $(wm size 2>/dev/null | tail -n 1)"
echo "密度: $(wm density 2>/dev/null | tail -n 1)"
echo "mWakefulness: $(dumpsys power 2>/dev/null | grep -m1 mWakefulness=)"
echo "mScreenState: $(dumpsys display 2>/dev/null | grep -m1 mScreenState)"
echo "deviceidle: $(dumpsys deviceidle get screen 2>/dev/null)"
echo "背光节点:"
for f in /sys/class/backlight/*/brightness; do
  [ -e "$f" ] && echo "  $f = $(cat "$f" 2>/dev/null)"
done
'

grab "前台应用" '
echo "mCurrentFocus: $(dumpsys window 2>/dev/null | grep -m1 mCurrentFocus)"
echo "topResumedActivity: $(dumpsys activity activities 2>/dev/null | grep -m1 -i topResumedActivity)"
'

head_line "输入设备"
if [ -r /proc/bus/input/devices ]; then
	grep -E "^(N|H|B): " /proc/bus/input/devices >>"${REPORT}" 2>&1
else
	echo "无法读取 /proc/bus/input/devices" >>"${REPORT}"
fi

head_line "模块配置"
if [ -f "${MOD}/配置.prop" ]; then
	cat "${MOD}/配置.prop" >>"${REPORT}" 2>&1
else
	echo "未找到 ${MOD}/配置.prop" >>"${REPORT}"
fi

head_line "配置的应用是否已安装"
if [ -f "${MOD}/配置.prop" ]; then
	grep -E '^[[:space:]]*package=' "${MOD}/配置.prop" 2>/dev/null | sed 's/^[^=]*=//' | tr -d ' ' | while read -r p; do
		[ -n "$p" ] || continue
		if pm path "$p" >/dev/null 2>&1; then
			echo "已安装: $p"
		else
			echo "未安装: $p"
		fi
	done >>"${REPORT}" 2>&1
	echo "第三方应用总数: $(pm list packages -3 2>/dev/null | wc -l)" >>"${REPORT}"
else
	echo "未找到配置" >>"${REPORT}"
fi

head_line "最近日志（200 行）"
if [ -f "${MOD}/运行日志.log" ]; then
	tail -n 200 "${MOD}/运行日志.log" >>"${REPORT}" 2>&1
else
	echo "未找到运行日志" >>"${REPORT}"
fi

head_line "模块自检"
if [ -x "${BIN}" ]; then
	"${BIN}" --diag --config="${MOD}/配置.prop" >>"${REPORT}" 2>&1
else
	echo "未找到可执行的 ${BIN}" >>"${REPORT}"
fi

raw "dumpsys_power.txt" "dumpsys power"
raw "dumpsys_display.txt" "dumpsys display"
raw "dumpsys_window.txt" "dumpsys window"
raw "dumpsys_battery.txt" "dumpsys battery"
raw "input_devices.txt" "cat /proc/bus/input/devices"

PKG=""
if command -v zip >/dev/null 2>&1; then
	(cd "${OUT_DIR}" && zip -qr "anmian_diag_${TS}.zip" "anmian_diag_${TS}" "anmian_diag_${TS}.txt" 2>/dev/null) && PKG="${OUT_DIR}/anmian_diag_${TS}.zip"
fi

say ""
say "诊断完成，把下面文件发给开发者即可："
say "  报告: ${REPORT}"
say "  原始数据: ${WORK}/"
[ -n "${PKG}" ] && say "  打包: ${PKG}"

exit 0
