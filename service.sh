#!/system/bin/sh

MODDIR="${0%/*}"
[ "${MODDIR}" = "$0" ] && MODDIR="."
cd "${MODDIR}" 2>/dev/null || exit 1

debug=true

PIDFILE="${MODDIR}/.supervisor.pid"
BOOTID_FILE="/proc/sys/kernel/random/boot_id"

boot_id() {
	cat "${BOOTID_FILE}" 2>/dev/null
}

kill_old_supervisor() {
	[ -f "${PIDFILE}" ] || return 0
	local bid pid
	read -r bid pid <"${PIDFILE}" 2>/dev/null || { rm -f "${PIDFILE}"; return 0; }
	if [ -n "${bid}" ] && [ "${bid}" != "$(boot_id)" ]; then
		rm -f "${PIDFILE}"
		return 0
	fi
	[ -n "${pid}" ] && kill "${pid}" 2>/dev/null
	rm -f "${PIDFILE}"
}

wait_sys_boot_completed() {
	local i=9
	until [ "$(getprop sys.boot_completed)" = "1" ] || [ "${i}" -le 0 ]; do
		i=$((i - 1))
		sleep 25
	done
}

wait_sys_boot_completed

kill_old_supervisor
killall -15 App_screen_off >/dev/null 2>&1

[ -f "${MODDIR}/App_screen_off" ] || exit 1
chmod 0755 "${MODDIR}/App_screen_off" >/dev/null 2>&1

module_config="${MODDIR}/配置.prop"
log_file="${MODDIR}/运行日志.log"

export MODDIR module_config log_file debug

nohup sh -c '
	while true; do
		"${MODDIR}/App_screen_off" --config="${module_config}" --log="${log_file}" --debug="${debug}"
		if [ "${debug}" = "true" ]; then
			echo "[$(date "+%Y-%m-%d %H:%M:%S")] [W] 守护进程已退出，60 秒后重启" >>"${log_file}" 2>/dev/null
		fi
		sleep 60
	done
' >/dev/null 2>&1 &

echo "$(boot_id) $!" >"${PIDFILE}"
