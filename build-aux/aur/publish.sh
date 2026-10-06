#!/bin/sh
# The AUR's augur-dbus, for a release on GitHub: run as root on Arch (a
# container, in CI), from the top of the source.
#
#   build-aux/aur/publish.sh VERSION [--dry-run]
#
# The PKGBUILD here gets the version and the tarball's checksum (from the
# release's SHA256SUMS), is built and tested with makepkg from the
# published tarball, and then it and its .SRCINFO are pushed to the AUR
# with the key in AUR_SSH_KEY. --dry-run stops before the push, and needs
# no key.
set -eu

version=$1
dry=${2:-}
release="https://github.com/matthewp/augur/releases/download/v$version"
tarball="augur-$version.tar.xz"
# The AUR's own host key, as its home page gives it: never trust on first
# use.
host_key='aur.archlinux.org ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEuBKrPzbawxA/k2g6NcyV5jmqwJ2s+zpgZGZ7tpLIcN'

if [ "$dry" != "--dry-run" ] && [ -z "${AUR_SSH_KEY:-}" ]; then
	echo "publish.sh: AUR_SSH_KEY isn't set (the AUR_SSH_KEY secret, in CI)" >&2
	exit 1
fi

pacman -Syu --noconfirm --needed base-devel git openssh meson glib2 \
	json-glib libsoup3 python dbus

sum=$(curl -sfL "$release/SHA256SUMS" |
	awk -v f="$tarball" '$2 == f { print $1 }')
if [ -z "$sum" ]; then
	echo "publish.sh: no checksum for $tarball in $release/SHA256SUMS" >&2
	exit 1
fi

# makepkg won't run as root.
id builder >/dev/null 2>&1 || useradd -m builder
work=/home/builder/augur-dbus
rm -rf "$work"
mkdir -p "$work"
sed -e "s/^pkgver=.*/pkgver=$version/" -e "s/^pkgrel=.*/pkgrel=1/" \
	-e "s/^sha256sums=.*/sha256sums=('$sum')/" build-aux/aur/PKGBUILD \
	> "$work/PKGBUILD"
chown -R builder "$work"
su builder -c "cd $work && makepkg --cleanbuild --noconfirm &&
	makepkg --printsrcinfo > .SRCINFO"
cat "$work/.SRCINFO"

if [ "$dry" = "--dry-run" ]; then
	echo "publish.sh: built; not pushed (--dry-run)"
	exit 0
fi

ssh=$(mktemp -d)
printf '%s\n' "$AUR_SSH_KEY" > "$ssh/key"
chmod 600 "$ssh/key"
printf '%s\n' "$host_key" > "$ssh/known_hosts"
export GIT_SSH_COMMAND="ssh -i $ssh/key -o IdentitiesOnly=yes -o UserKnownHostsFile=$ssh/known_hosts -o StrictHostKeyChecking=yes"

repo=$(mktemp -d)
git clone ssh://aur@aur.archlinux.org/augur-dbus.git "$repo"
cp "$work/PKGBUILD" "$work/.SRCINFO" "$repo/"
cd "$repo"
git add PKGBUILD .SRCINFO
if git diff --cached --quiet; then
	echo "publish.sh: the AUR already has augur-dbus $version"
else
	git -c user.name="Matthew Phillips" \
		-c user.email="matthew@matthewphillips.info" \
		commit -m "augur-dbus $version"
	git push origin HEAD:master
fi
rm -rf "$ssh"
