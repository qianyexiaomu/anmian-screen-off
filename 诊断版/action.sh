#!/system/bin/sh
MODDIR="${0%/*}"
[ "${MODDIR}" = "$0" ] && MODDIR="/data/adb/modules/anmian_screen_off"
sh "${MODDIR}/collect.sh"
