# FreeBSD notes

Augur builds and runs on FreeBSD. This was checked on FreeBSD 15.1-RELEASE
(amd64, clang 19.1.7), at commit `4efa37a` from GitHub, on 2026-10-05.

## Results

- `meson setup build && ninja -C build` builds cleanly, with no errors and no warnings.
- `meson test`: the `schema` test passes. The `service` test passes 111 of
  its 115 checks. The 4 failures come from GNU-only commands in
  `tests/test_service.sh`, not from augur itself.

## What's needed

### Packages

    pkg install git meson ninja pkgconf glib json-glib libsoup3   # build
    pkg install dbus python3                                      # service test

`python3` is a separate package. `python312` only installs `python3.12`,
and the test script calls `python3` by name. Meson doesn't catch this,
because `find_program('python3')` falls back to the Python that meson
itself runs on.

### systemd

FreeBSD has no systemd. Configure with `-Dsystemduserunitdir=no`, or the
build installs an unused `augurd.service` under `$prefix/lib/systemd/user`.

### Portability fixes for tests/test_service.sh

FreeBSD has BSD `sed` and `head`, and these lines rely on GNU behaviour:

- `sed -i 's/…/' "$CONFIG"`: BSD sed takes the next argument after `-i` as
  the backup suffix, so the edit never happens. Use `sed -i.bak …`, which
  works with both GNU and BSD sed. This breaks "a bad price says so", "the
  config turns it off" and "asking while off".
- The `enabled = false` edit also uses `\n` in a sed replacement. That is a
  GNU extension; BSD sed won't put a newline there. Append the line another
  way, for example by rewriting the file with `awk` or `printf`.
- `head -n -1 "$TMP/requests" | tail -n 1`: BSD head rejects negative
  counts. Use `tail -n 2 "$TMP/requests" | head -n 1` instead. This breaks
  "... any tool, the answer one included".

macOS has the same BSD tools, so these fixes help there too.

## The test VM

There is a FreeBSD 15.1 VM on this machine at `~/vms/freebsd/`. It has 4
CPUs, 4 GB of RAM, a 30 GB disk and user-mode NAT.

- **Start:** `~/vms/freebsd/start.sh` starts it headless in the background.
  It writes `vm.pid` and refuses to start a second copy. SSH comes up
  roughly 30 to 60 seconds later.
- **SSH:** `ssh -p 2222 -o BatchMode=yes root@localhost '<command>'`. It
  uses key auth with `~/.ssh/id_ed25519`. The port listens only on
  127.0.0.1.
- **Stop:** `ssh -p 2222 root@localhost 'shutdown -p now'`. **Always stop it
  this way.** Killing QEMU loses recent writes on the guest's UFS disk.
  That has already happened once: the SSH host keys and `authorized_keys`
  were lost, and SSH failed with "REMOTE HOST IDENTIFICATION HAS CHANGED".
- **Console:** the serial console is on `~/vms/freebsd/console.sock`
  (`socat - UNIX-CONNECT:…`; socat isn't installed on the host). Root has
  no password at the console. For an interactive session on the
  terminal's serial console, use `~/vms/freebsd/run.sh` instead of
  `start.sh`; exit with `Ctrl-a x`, or better, `shutdown -p now`.
- **Code:** the VM has a clone at `/root/augur`, with the packages above
  installed and `build/` configured. It tracks GitHub, not this working
  tree. To test local, uncommitted changes, copy them over (rsync isn't
  installed in the VM):

      tar czf - --exclude=.git --exclude=build --exclude=.cache . \
        | ssh -p 2222 root@localhost 'rm -rf /root/augur-local && mkdir /root/augur-local && tar xzf - -C /root/augur-local'
      ssh -p 2222 root@localhost 'cd /root/augur-local && meson setup build && ninja -C build && meson test -C build --print-errorlogs'

- **Clock:** the guest clock falls behind after the host sleeps; once it
  was about 16 hours off. If `pkg` or `git` fail with TLS or certificate
  errors, fix the clock with `ntpdate pool.ntp.org`.
