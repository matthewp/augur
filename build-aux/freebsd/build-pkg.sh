#!/bin/sh
# A FreeBSD package, built on the FreeBSD it's for (a VM, in CI), as root,
# from the top of the source:
#
#   build-aux/freebsd/build-pkg.sh OUTDIR
#
# It installs what it needs with pkg, builds and tests Augur, installs it
# into a staging tree, and makes augur-VERSION-freebsdN-ARCH.pkg in OUTDIR:
# for this system's ABI (pkg config abi), which pkg insists on. The
# libraries it depends on are recorded at the versions it was built with.
set -eu

out=$(mkdir -p "$1" && cd "$1" && pwd)
version=$(sed -n "s/^	version: '\(.*\)',/\1/p" meson.build)
# What augurd needs at run time: the libraries, TLS for libsoup (which
# is glib-networking), and a session bus.
deps="glib json-glib libsoup3 glib-networking dbus"

export ASSUME_ALWAYS_YES=yes
pkg install -y meson ninja pkgconf python3 $deps

build=$(mktemp -d)
stage=$(mktemp -d)
meson setup "$build" --prefix=/usr/local --buildtype=release \
	-Dsystemduserunitdir=no
meson compile -C "$build"
meson test -C "$build" --print-errorlogs
DESTDIR="$stage" meson install -C "$build"

abi=$(pkg config abi)                 # e.g. FreeBSD:15:amd64
major=$(echo "$abi" | cut -d: -f2)
arch=$(echo "$abi" | cut -d: -f3)

# The files, as pkg's plist has them: relative to the prefix.
(cd "$stage/usr/local" && find . -type f | sed 's,^\./,,' | sort) \
	> "$build/plist"

{
	cat <<MANIFEST
name: augur
version: "$version"
origin: sysutils/augur
comment: "AI for desktop programs, as a session D-Bus service"
maintainer: matthew@matthewphillips.info
www: https://github.com/matthewp/augur
abi: "$abi"
prefix: /usr/local
licenselogic: single
licenses: [BSD3CLAUSE]
categories: [sysutils]
desc: <<EOD
Augur gives desktop applications AI without each of them talking to AI
providers. A program asks Augur over D-Bus; Augur knows the providers
(OpenAI, Anthropic, OpenRouter, Cloudflare AI Gateway, Ollama and any
OpenAI-compatible API), the keys, which model to use, and how to get a
well-formed answer out of each.

This package has the service, augurd, started by D-Bus when first asked,
and augur, a command line for asking it and checking its configuration.
EOD
deps: {
MANIFEST
	for d in $deps; do
		printf '  %s: { origin: "%s", version: "%s" }\n' "$d" \
			"$(pkg query %o "$d")" "$(pkg query %v "$d")"
	done
	echo "}"
} > "$build/+MANIFEST"

pkg create -M "$build/+MANIFEST" -r "$stage" -p "$build/plist" -o "$build"
cp "$build/augur-$version.pkg" "$out/augur-$version-freebsd$major-$arch.pkg"
ls -l "$out"
