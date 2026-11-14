[[ -f /data/adb/magisk/util_functions.sh ]] && . /data/adb/magisk/util_functions.sh
[[ ! -d "${MODPATH}" ]] && MODPATH="${0%/*}"
ui_print "" >/dev/null 2>&1 || alias ui_print="echo"
grep_prop >/dev/null 2>&1 || grep_prop() { local REGEX="s/^$1=//p"; shift; local FILES="${@:-/system/build.prop}"; for FILE in "${FILES[@]}"; do [ -f "$FILE" ] && cat "$FILE" 2>/dev/null | tr -d '\r' | sed -n "$REGEX" | head -n 1; done; }

function get_magisk_lite(){
local until_function=/data/adb/magisk/util_functions.sh
local Apatch_version=${APATCH_VER_CODE}
local Ksu_version=${KSU_KERNEL_VER_CODE}
if [ "${Apatch_version}" != "" ];then
	echo "- 😥当前环境非Magisk……"
	echo "- 疑似Apatch◎${Apatch_version}……"
	return
elif [ "${Ksu_version}" != "" ];then
	echo "- 😥当前环境非Magisk……"
	echo "- 疑似kernelsu◎${Ksu_version}……"
	return
fi
if grep -q lite_modules $until_function >/dev/null 2>&1 ;then
	echo "- 🌙当前为: Magisk Lite◎$MAGISK_VER_CODE"
else
	if [ "${MAGISK_VER_CODE}" = "" ];then
		echo "- 🤔当前环境非Magisk/Ksu/Apatch……"
	else
case "${MAGISK_VER}" in
*alpha)
	echo "- ☀当前为: Magisk Alpha◎$MAGISK_VER_CODE"
;;
*delta)
	echo "- ☀当前为: Magisk Delta◎$MAGISK_VER_CODE"
;;
*kitsune)
	local magisk_type="${MAGISK_VER%-*}"
	if [[ -n $(echo "${magisk_type}" | grep -E '[0-9]{2}\.[0-9]') ]] ;then
		echo "- ☀当前为: Kitsune Mask◎$MAGISK_VER_CODE"
	else
		echo "- ☀当前为: Kitsune Mask(${magisk_type})◎$MAGISK_VER_CODE"
	fi
;;
*-*)
	local magisk_others="$(echo "${MAGISK_VER##*-}" | tr '[:lower:]' '[:upper:]')"
	echo "- ☀当前为: Magisk ${magisk_others}◎$MAGISK_VER_CODE"
;;
*)
	echo "- ☀当前为: Magisk Official◎$MAGISK_VER_CODE"
;;
esac
	fi
fi
}

function cmp_bin_file(){
local original_bin_file="/data/adb/modules/anmian_screen_off/App_screen_off"
local update_bin_file="${original_bin_file/modules/modules_update}"
[ ! -f "${update_bin_file}" ] && return
if ! cmp "${original_bin_file}" "${update_bin_file}" >/dev/null 2>&1 ;then
	echo "- 升级 ${original_bin_file##*/} 文件……"
	cp -af "${update_bin_file}" "${original_bin_file}" >/dev/null 2>&1
fi
}

function Move_platform_bin(){
local platform="${ABI}"
[ ! -d "${MODPATH}" ] && return
[ "${platform}" = "" ] && platform="$(getprop ro.product.cpu.abi)"
[ "${platform}" = "" ] && platform="$(grep_prop ro.product.cpu.abi)"
if [ "${platform}" = "" ] ;then
	ui_print "- 无法获取您的设备架构"
	abort >/dev/null 2>&1
else
	local bin_file_name="App_screen_off"
	local bin_file="${MODPATH}/bin/${platform}/${bin_file_name}"
	local bin_Info="$(file "${bin_file}" | sed "s|${bin_file}:||g" | tr ',' '\n' | sed -E 's/^(.*)/-\1/g' )"
	ui_print "- 架构: ${bin_file##*/bin/} "
	ui_print "- 构建信息: 
${bin_Info} "
	cp -rf "${bin_file}" "${MODPATH}/${bin_file_name}"
	rm -rf "${MODPATH}/bin"
fi
}

function update_program_Running(){
if [ "${0##*/}" != "action.sh" ];then
	ui_print "- Something for nothing ……"
	Move_platform_bin
	[ -f "${TMPDIR}/module.prop" ] && rm -rf "${MODPATH}/installer.sh"
	local my_module_patch="/data/adb/modules/anmian_screen_off"
	[ -f "${my_module_patch}/配置.prop" ] && cp -af "${my_module_patch}/配置.prop" "${MODPATH}/配置.prop"
	rm -rf "${MODPATH}/源码" "$MODPATH/README.md" "$MODPATH/update.md" 
	mkdir -p "${my_module_patch}" && cp -af "${MODPATH}"/* "${my_module_patch}" && chmod -R 0775 "${my_module_patch}" && rm -rf "${my_module_patch}/installer.sh" "${my_module_patch}/customize.sh"
else
	echo "- nothing……"
fi
}


ui_print ""
ui_print "**************************************************"
ui_print "－品牌: `getprop ro.product.brand`"
ui_print "－代号: `getprop ro.product.device`"
ui_print "－模型: `getprop ro.product.model`"
ui_print "－安卓版本: `getprop ro.build.version.release`"
test "`getprop ro.miui.ui.version.name`" != "" && ui_print "－MIUI版本: MIUI `getprop ro.miui.ui.version.name` - `getprop ro.build.version.incremental` "
ui_print "－内核版本: `uname -a `"
ui_print "－运存大小: `free -m|grep "Mem"|awk '{print $2}'`MB  已用:`free -m|grep "Mem"|awk '{print $3}'`MB  剩余:$((`free -m|grep "Mem"|awk '{print $2}'`-`free -m|grep "Mem"|awk '{print $3}'`))MB"
ui_print "－Swap大小: `free -m|grep "Swap"|awk '{print $2}'`MB  已用:`free -m|grep "Swap"|awk '{print $3}'`MB  剩余:`free -m|grep "Swap"|awk '{print $4}'`MB"
ui_print "**************************************************"
ui_print "- 模块信息:"
ui_print "$(get_magisk_lite)"
ui_print "- Module version: $(grep_prop version "${MODPATH}/module.prop")"
ui_print "- Module versionCode: $(grep_prop versionCode "${MODPATH}/module.prop")"
ui_print "- 描述: $(grep_prop description "${MODPATH}/module.prop")"

ui_print ""
ui_print "**************************************************"
ui_print ""
update_program_Running
ui_print ""
ui_print "**************************************************"



