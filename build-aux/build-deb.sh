#!/bin/sh
# The .deb, built on the Debian or Ubuntu it's for (a container, in CI):
#
#   build-aux/build-deb.sh OUTDIR      from the top of the source
#
# The version gets the system's name and release (0.1.0-1~debian12,
# 0.1.0-1~ubuntu24.04), so each one's package is its own and a newer
# release's sorts after it. Built from a copy: the source isn't changed.
set -eu

out=$(mkdir -p "$1" && cd "$1" && pwd)
. /etc/os-release
suffix="~${ID}${VERSION_ID}"

upstream=$(sed -n "s/^\tversion: '\(.*\)',/\1/p" meson.build)
debian=$(sed -n '1s/^[^(]*(\([^-)]*\).*/\1/p' debian/changelog)
if [ "$upstream" != "$debian" ]; then
	echo "build-deb.sh: meson.build says $upstream, debian/changelog $debian" >&2
	exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q --no-install-recommends build-essential dpkg-dev
apt-get build-dep -y -q ./

work=$(mktemp -d)
mkdir "$work/augur"
tar -c --exclude=./.git --exclude=./build . | tar -x -C "$work/augur"
cd "$work/augur"
sed -i "1s/(\([^)]*\))/(\1$suffix)/" debian/changelog
dpkg-buildpackage -us -uc -b
cp ../augur-dbus_*.deb "$out/"
ls -l "$out"
