if [ ! -f "${MODPATH}/配置.prop" ]; then
	for f in "${MODPATH}"/*.prop; do
		[ -f "$f" ] || continue
		case "${f##*/}" in
		module.prop) continue ;;
		esac
		mv -f "$f" "${MODPATH}/配置.prop" && echo "- 已修正配置文件名为 配置.prop"
		break
	done
fi

abi="$(getprop ro.product.cpu.abi 2>/dev/null)"
[ -z "${abi}" ] && abi="${ABI}"
bin_src="${MODPATH}/bin/${abi}/App_screen_off"
if [ ! -f "${bin_src}" ]; then
	bin_src="$(ls "${MODPATH}"/bin/*/App_screen_off 2>/dev/null | head -n 1)"
fi
if [ -f "${bin_src}" ]; then
	cp -f "${bin_src}" "${MODPATH}/App_screen_off"
	chmod 0755 "${MODPATH}/App_screen_off"
fi

source "${MODPATH}/installer.sh"
chmod -R 0775 "${MODPATH}"

if [ ! -f "${MODPATH}/App_screen_off" ]; then
	ui_print "- 警告：没有找到 App_screen_off，模块无法运行，请反馈设备架构：${abi}"
fi
