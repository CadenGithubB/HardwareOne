# Sourced by tools/build_board.sh and tools/build_deployment.sh.
#
# Every build directory of one checkout shares <repo>/managed_components (and
# updater/managed_components), which the component manager rewrites to the
# lock of whichever target configures. Builds of different targets one after
# another are fine (cmake/HW1ManagedComponents.cmake makes the next build
# resync; the updater locks pin the same versions for every target, so its
# directory never changes), and builds of the same target may run together
# (they use the same lock, and the sync itself is serialized by a CMake file
# lock). Two builds of different targets at once would change each other's
# sources mid-compile, so the second is refused instead.
#
# Implementation: a shared flock(2) on <repo>/.hw1-build/checkout.lock held
# through file descriptor 9 of the calling script, joined only by builds of the
# target recorded in it; checkout.mutex makes the check-and-join atomic. The
# lock belongs to the script's open file, so it is released when the script
# and its children exit, however they exit, and never goes stale. A script run
# by a build that already holds it (HW1_CHECKOUT_BUILD_LOCK set) does not take
# it again.
#
#   hw1_take_checkout_build_lock <repo> <idf target> <description>

hw1_take_checkout_build_lock() {
  local repo="$1" target="$2" what="$3"
  [[ -n "${HW1_CHECKOUT_BUILD_LOCK:-}" ]] && return 0
  mkdir -p "$repo/.hw1-build"
  local lock="$repo/.hw1-build/checkout.lock"
  exec 9>>"$lock"
  if ! python3 -c '
import fcntl, os, sys
lock, target, what = sys.argv[1:4]
me = "pid %d: %s" % (os.getppid(), what)
with open(os.path.join(os.path.dirname(lock), "checkout.mutex"), "a") as mutex:
    fcntl.flock(mutex, fcntl.LOCK_EX)
    try:
        fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        with open(lock) as f:
            lines = f.read().splitlines()
        held = lines[0] if lines else ""
        if held != target:
            sys.stderr.write(
                "error: an %s build is running in this checkout (%s).\n"
                "       All builds of one checkout share managed_components/, synced to the\n"
                "       building target'"'"'s lock, so builds of different targets run one at a\n"
                "       time. Wait for it, or build %s in a separate git worktree.\n"
                % (held or "unknown-target", "; ".join(lines[1:]) or "holder unknown",
                   target))
            sys.exit(1)
        fcntl.flock(9, fcntl.LOCK_SH | fcntl.LOCK_NB)
        os.write(9, (me + "\n").encode())
    else:
        os.ftruncate(9, 0)
        os.write(9, ("%s\n%s\n" % (target, me)).encode())
        fcntl.flock(9, fcntl.LOCK_SH)
' "$lock" "$target" "$what"; then
    exit 1
  fi
  export HW1_CHECKOUT_BUILD_LOCK="$lock"
}
