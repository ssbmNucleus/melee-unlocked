#!/bin/sh
# Nucleus Unlocked for Linux: starts the game with this folder's Sys files, settings, memory card and replays
# (tools/package_linux_release.py ships it at the top of the release folder).
#   ./Nucleus.sh                      the disc chosen last time, else melee.iso in this folder, else a
#                                     file picker (zenity or kdialog); the choice is remembered
#   ./Nucleus.sh /path/to/melee.iso   that disc (remembered too), then any other options
#   ./Nucleus.sh --settings           the settings window
#   ./Nucleus.sh --other-options      melee_source's own options with this folder's places (--help lists them)
# SPDX-License-Identifier: GPL-2.0-or-later
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd) || exit 1
cd "$here" || exit 1
remembered="$here/User/disc-path"

# A message the player sees: on the terminal, and in a window when there is no terminal (started from
# a file manager or a desktop launcher).
say() {
  printf '%s\n' "$1" >&2
  if [ ! -t 2 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    if command -v zenity >/dev/null 2>&1; then
      zenity --info --no-markup --title="Nucleus Unlocked" --text="$1" >/dev/null 2>&1
    elif command -v kdialog >/dev/null 2>&1; then
      kdialog --title "Nucleus Unlocked" --msgbox "$1" >/dev/null 2>&1
    fi
  fi
}

# Melee NTSC 1.02: game ID GALE01 at the start of the disc, disc revision 2 at byte 7.
is_melee() {
  [ -f "$1" ] && [ -r "$1" ] || return 1
  [ "$(head -c 6 -- "$1" 2>/dev/null)" = GALE01 ] || return 1
  [ "$(od -An -tu1 -j7 -N1 -- "$1" 2>/dev/null | tr -d ' \n')" = 2 ]
}

# The file the player picks, or nothing (cancelled, or no picker on this desktop).
pick() {
  if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    return 0
  elif command -v zenity >/dev/null 2>&1; then
    zenity --file-selection --title="$1" --file-filter="Melee disc (ISO, GCM) | *.iso *.ISO *.gcm *.GCM" \
      --file-filter="All files | *" 2>/dev/null
  elif command -v kdialog >/dev/null 2>&1; then
    kdialog --title "$1" --getopenfilename "$HOME" "*.iso *.ISO *.gcm *.GCM|Melee disc (ISO, GCM)" 2>/dev/null
  fi
}

remember() {
  mkdir -p "$here/User" 2>/dev/null && printf '%s\n' "$1" > "$remembered.$$" 2>/dev/null &&
    mv -f "$remembered.$$" "$remembered" 2>/dev/null
  rm -f "$remembered.$$" 2>/dev/null
  return 0
}

# The game and its library are built for AVX2; a processor without it would stop on the first
# instruction with no message. The compatibility build, when this folder has one, runs instead.
game="$here/melee_source"
if ! grep -qw avx2 /proc/cpuinfo 2>/dev/null; then
  if [ -x "$here/melee_source_compat" ]; then
    game="$here/melee_source_compat"
  else
    say "Nucleus Unlocked needs a processor with AVX2 (Intel Core 4th generation, 2013, or newer; any AMD Ryzen). This processor does not have it."
    exit 3
  fi
fi

# melee_source with this folder's places, then the given options.
run() {
  exec "$game" --settings-path "$here/port-settings.ini" --sys-dir "$here/Sys" --user-dir "$here/User/Slippi" \
    --replay-dir "$here/Replays" --card-dir "$here/User/GC/CardA" "$@"
}

case "${1:-}" in
  --help|-h|--version|--print-info) exec "$game" "$@" ;;
  --settings) shift; run --settings-window "$@" ;;
  -*) run "$@" ;;
  "") ;;
  *)
    disc=$1; shift
    if ! is_melee "$disc"; then
      say "$disc is not a Melee NTSC 1.02 disc image. Nucleus Unlocked needs your own Melee NTSC 1.02 ISO (GALE01, revision 2)."
      exit 2
    fi
    remember "$disc"
    run --iso "$disc" "$@" ;;
esac

# No disc named: the one chosen last time, melee.iso beside this file, else ask.
disc=""
if [ -f "$remembered" ]; then
  disc=$(head -n 1 -- "$remembered")
fi
if ! is_melee "$disc"; then
  disc="$here/melee.iso"
fi
title="Choose your Melee NTSC 1.02 disc image (ISO)"
while ! is_melee "$disc"; do
  if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] || { ! command -v zenity >/dev/null 2>&1 && ! command -v kdialog >/dev/null 2>&1; }; then
    say "Nucleus Unlocked needs your own Melee NTSC 1.02 disc image. Start it with the disc's path: ./Nucleus.sh /path/to/melee.iso (or put the disc in this folder as melee.iso)."
    exit 2
  fi
  disc=$(pick "$title")
  if [ -z "$disc" ]; then
    say "Nucleus Unlocked needs your own Melee NTSC 1.02 disc image to start. Start it again and choose the file, or start it with: ./Nucleus.sh /path/to/melee.iso"
    exit 2
  fi
  title="That file is not Melee NTSC 1.02. Choose your Melee NTSC 1.02 ISO"
done
remember "$disc"
run --iso "$disc"
