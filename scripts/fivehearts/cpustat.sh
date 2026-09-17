#!/bin/sh
# Sample the CPU clock and load, to tell "our code is slow" apart from
# "this SoC is running at 480 MHz right now".
#
#   sh cpustat.sh [samples] [interval_seconds]
#
# Watch it while the game plays: if scaling_cur_freq sits well below
# cpuinfo_max_freq, no amount of bridge tuning will make 720p video smooth, and
# the fix is the governor / a lighter stream instead of more software work.
SAMPLES="${1:-5}"
INTERVAL="${2:-1}"

sample() {
    line=""
    for c in /sys/devices/system/cpu/cpu[0-9]*; do
        cur=$(cat "$c/cpufreq/scaling_cur_freq" 2>/dev/null)
        [ -n "$cur" ] || continue
        line="$line $(basename "$c")=$((cur / 1000))MHz"
    done
    gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)
    max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq 2>/dev/null)
    temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    echo "gov=$gov max=$((max / 1000))MHz$line load=$(cut -d' ' -f1-3 /proc/loadavg) temp=$temp"
}

i=0
while [ "$i" -lt "$SAMPLES" ]; do
    sample
    i=$((i + 1))
    [ "$i" -lt "$SAMPLES" ] && sleep "$INTERVAL"
done
