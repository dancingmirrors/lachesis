#!/bin/sh

export LC_ALL=C

version_h=""
extra=""
cwd=""

for ac_option do
  ac_arg=$(echo "$ac_option" | cut -d '=' -f 2-)
  case "$ac_option" in
  --extra=*)
    extra="-$ac_arg"
    ;;
  --versionh=*)
    case "$ac_arg" in
      /*) version_h="$ac_arg" ;;
      *)  version_h="$(pwd)/$ac_arg" ;;
    esac
    ;;
  --cwd=*)
    cwd="$ac_arg"
    ;;
  *)
    echo "Unknown parameter: $ac_option" >&2
    exit 1
    ;;
  esac
done

if test -n "$cwd" ; then
  cd "$cwd" || exit 1
fi

version="$(git -c safe.directory='*' describe --match "v[0-9]*" --tags --long --always --abbrev=40 --dirty 2>/dev/null)"

if test -z "$version_h" ; then
  echo "${version:-UNKNOWN}${extra}"
  exit 0
fi

if test -z "$version" ; then
  if test -f "$version_h" ; then
    exit 0
  fi
  version="UNKNOWN"
fi

NEW_REVISION="#define VERSION \"${version}${extra}\""
OLD_REVISION=$(head -n 1 "$version_h" 2> /dev/null)

if test "$NEW_REVISION" != "$OLD_REVISION" ; then
  printf '%s\n' "$NEW_REVISION" > "$version_h"
fi
