#!/bin/sh
# A package into Augur's FreeBSD repository, served from pkg.gemwm.org
# (an R2 bucket), signed: run as root on the FreeBSD the package is for (a
# VM, in CI), from the top of the source.
#
#   build-aux/freebsd/publish.sh PKG [--local DIR]
#
# The repository for this system's ABI (augur/freebsd/FreeBSD:15:amd64/,
# say) is copied down, the package goes in its All/ beside the older ones,
# pkg repo writes and signs a new index, and it's all copied back; the
# public key goes beside the repositories as augur/freebsd/augur.pub.
# --local DIR does it in DIR instead of the bucket, to try it.
#
# PKG_SIGNING_KEY: the repository's private key (RSA, PEM).
# R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, R2_ENDPOINT (the account's S3
# endpoint) and R2_BUCKET: the bucket's, not needed with --local.
set -eu

pkgfile=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
local_dir=
if [ "${2:-}" = --local ]; then
	local_dir=$(mkdir -p "$3" && cd "$3" && pwd)
fi
pubkey=$(pwd)/build-aux/freebsd/augur.pub

if [ ! -f "$pubkey" ]; then
	echo "publish.sh: no build-aux/freebsd/augur.pub: the signing key's public half goes there" >&2
	exit 1
fi
if [ -z "${PKG_SIGNING_KEY:-}" ]; then
	echo "publish.sh: PKG_SIGNING_KEY isn't set" >&2
	exit 1
fi
abi=$(pkg config abi)
work=$(mktemp -d)
key="$work/key.pem"
(umask 077 && printf '%s\n' "$PKG_SIGNING_KEY" > "$key")

if [ -n "$local_dir" ]; then
	fetch_repo() { mkdir -p "$local_dir/$abi" && cp -R "$local_dir/$abi/." "$work/repo/"; }
	put_repo() { cp -R "$work/repo/." "$local_dir/$abi/"; cp "$pubkey" "$local_dir/augur.pub"; }
else
	for v in R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY R2_ENDPOINT R2_BUCKET; do
		eval "[ -n \"\${$v:-}\" ]" || { echo "publish.sh: $v isn't set" >&2; exit 1; }
	done
	ASSUME_ALWAYS_YES=yes pkg install -y rclone
	export RCLONE_CONFIG_R2_TYPE=s3 RCLONE_CONFIG_R2_PROVIDER=Cloudflare \
		RCLONE_CONFIG_R2_REGION=auto \
		RCLONE_CONFIG_R2_ACCESS_KEY_ID="$R2_ACCESS_KEY_ID" \
		RCLONE_CONFIG_R2_SECRET_ACCESS_KEY="$R2_SECRET_ACCESS_KEY" \
		RCLONE_CONFIG_R2_ENDPOINT="$R2_ENDPOINT"
	remote="r2:$R2_BUCKET/augur/freebsd"
	fetch_repo() { rclone copy "$remote/$abi" "$work/repo" --s3-no-check-bucket; }
	# Only this ABI's prefix is synced: the other version's job can't be
	# undone by this one.
	put_repo() {
		rclone sync "$work/repo" "$remote/$abi" --checksum --s3-no-check-bucket
		rclone copyto "$pubkey" "$remote/augur.pub" --checksum --s3-no-check-bucket
	}
fi

mkdir -p "$work/repo"
fetch_repo
mkdir -p "$work/repo/All"
cp "$pkgfile" "$work/repo/All/"
pkg repo "$work/repo" "$key"
put_repo
rm -f "$key"
echo "publish.sh: $(basename "$pkgfile") is in the $abi repository"
