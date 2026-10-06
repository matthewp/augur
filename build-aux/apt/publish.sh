#!/bin/sh
# A release's .debs into Augur's apt repository, served from pkg.gemwm.org
# (an R2 bucket), signed: run on Debian or Ubuntu (a GitHub runner, in
# CI), from the top of the source, as a user who may sudo apt-get.
#
#   build-aux/apt/publish.sh DEBDIR [--local DIR]
#
# Each .deb goes to the suite its version names (~debian12: bookworm,
# ~debian13: trixie, ~ubuntu24.04: noble), in pool/SUITE/ beside the older
# ones, under its proper name (GitHub's release assets have . for ~); then each suite's index is written again with apt-ftparchive and
# signed (InRelease, and Release.gpg for older apt), and the public key is
# put beside them as augur.gpg. The repository is copied down first and
# back after: nothing is kept anywhere else. --local DIR does it in DIR
# instead of the bucket, to try it.
#
# APT_SIGNING_KEY: the repository's private OpenPGP key (armored, no
# passphrase). R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY, R2_ENDPOINT and
# R2_BUCKET: the bucket's, not needed with --local.
set -eu

debdir=$(cd "$1" && pwd)
local_dir=
if [ "${2:-}" = --local ]; then
	local_dir=$(mkdir -p "$3" && cd "$3" && pwd)
fi
pubkey=$(pwd)/build-aux/apt/augur.gpg

if [ ! -f "$pubkey" ]; then
	echo "publish.sh: no build-aux/apt/augur.gpg: the signing key's public half goes there" >&2
	exit 1
fi
if [ -z "${APT_SIGNING_KEY:-}" ]; then
	echo "publish.sh: APT_SIGNING_KEY isn't set" >&2
	exit 1
fi

# The suite for a version, by the system it was built for.
suite_of() {
	case $1 in
	*~debian12) echo bookworm ;;
	*~debian13) echo trixie ;;
	*~ubuntu24.04) echo noble ;;
	*) echo "publish.sh: which suite is version $1 for?" >&2; return 1 ;;
	esac
}

command -v apt-ftparchive >/dev/null || sudo apt-get install -y -q apt-utils
work=$(mktemp -d)
export GNUPGHOME="$work/gnupg"
mkdir -m 700 "$GNUPGHOME"
printf '%s\n' "$APT_SIGNING_KEY" | gpg --batch --quiet --import
key=$(gpg --batch --list-secret-keys --with-colons | awk -F: '$1 == "fpr" { print $10; exit }')

if [ -n "$local_dir" ]; then
	fetch_repo() { cp -R "$local_dir/." "$work/repo/"; }
	put_repo() { cp -R "$work/repo/." "$local_dir/"; }
else
	for v in R2_ACCESS_KEY_ID R2_SECRET_ACCESS_KEY R2_ENDPOINT R2_BUCKET; do
		eval "[ -n \"\${$v:-}\" ]" || { echo "publish.sh: $v isn't set" >&2; exit 1; }
	done
	command -v rclone >/dev/null || sudo apt-get install -y -q rclone
	export RCLONE_CONFIG_R2_TYPE=s3 RCLONE_CONFIG_R2_PROVIDER=Cloudflare \
		RCLONE_CONFIG_R2_REGION=auto \
		RCLONE_CONFIG_R2_ACCESS_KEY_ID="$R2_ACCESS_KEY_ID" \
		RCLONE_CONFIG_R2_SECRET_ACCESS_KEY="$R2_SECRET_ACCESS_KEY" \
		RCLONE_CONFIG_R2_ENDPOINT="$R2_ENDPOINT"
	remote="r2:$R2_BUCKET/augur/debian"
	fetch_repo() { rclone copy "$remote" "$work/repo" --s3-no-check-bucket; }
	put_repo() { rclone sync "$work/repo" "$remote" --checksum --s3-no-check-bucket; }
fi

mkdir -p "$work/repo"
fetch_repo
for deb in "$debdir"/*.deb; do
	package=$(dpkg-deb -f "$deb" Package)
	version=$(dpkg-deb -f "$deb" Version)
	arch=$(dpkg-deb -f "$deb" Architecture)
	suite=$(suite_of "$version")
	mkdir -p "$work/repo/pool/$suite"
	cp "$deb" "$work/repo/pool/$suite/${package}_${version}_${arch}.deb"
done

cd "$work/repo"
for suite in bookworm trixie noble; do
	[ -d "pool/$suite" ] || continue
	rm -rf "dists/$suite"
	for arch in amd64 arm64; do
		dir="dists/$suite/main/binary-$arch"
		mkdir -p "$dir"
		apt-ftparchive --arch "$arch" packages "pool/$suite" > "$dir/Packages"
		gzip -9nk "$dir/Packages"
	done
	apt-ftparchive \
		-o APT::FTPArchive::Release::Origin=Augur \
		-o APT::FTPArchive::Release::Label=Augur \
		-o APT::FTPArchive::Release::Suite="$suite" \
		-o APT::FTPArchive::Release::Codename="$suite" \
		-o APT::FTPArchive::Release::Architectures="amd64 arm64" \
		-o APT::FTPArchive::Release::Components=main \
		-o APT::FTPArchive::Release::Description="Augur, from augur.gemwm.org" \
		release "dists/$suite" > "$work/Release"
	mv "$work/Release" "dists/$suite/Release"
	gpg --batch --yes --local-user "$key" --clearsign \
		-o "dists/$suite/InRelease" "dists/$suite/Release"
	gpg --batch --yes --local-user "$key" --detach-sign --armor \
		-o "dists/$suite/Release.gpg" "dists/$suite/Release"
done
cp "$pubkey" augur.gpg
cd - >/dev/null
put_repo
rm -rf "$GNUPGHOME"
echo "publish.sh: published $(ls "$debdir"/*.deb | wc -l) .debs"
