#!/usr/bin/env bash
# Single entry point for every build, test and packaging step. Requires only
# Docker on the host. Usage:
#
#   scripts/docker.sh image              build the pinned toolchain + host-test images
#   scripts/docker.sh lock               regenerate containers/toolchain.lock
#   scripts/docker.sh build debug|release
#   scripts/docker.sh test host          ARMv6 emitter + JIT probe logic under qemu-arm
#   scripts/docker.sh test x86 [golden]  x86 fixtures on qemu-i386 vs the golden file
#   scripts/docker.sh test cpu [armv7|armv6]  the same fixtures through the Box86 backend
#   scripts/docker.sh test m3            a PE32 through the Win32 runtime on the ARMv6 backend
#   scripts/docker.sh test tls           the reports' TLS module under qemu-arm: a test event to Sentry (network)
#   scripts/docker.sh test pad           the pad's readings between two message pumps (issue #118)
#   scripts/docker.sh test d2d [<game-dir>]   d2d's work tree and its harness's modules; with Game.exe, its cases vs our natives
#   scripts/docker.sh test aot <game-dir> [<save-dir>]   every AOT-compiled function checked against Box86 on the lane's
#                                        town walk and fight, and with an Act I save (dist/save/BeforeCh1End/save) Act II
#   scripts/docker.sh package           release build -> dist/<build-id>/ with manifest
#   scripts/docker.sh package-cia [<game-dir>]   the hardware build: dist/cia/3ds-d2-<version>-<build-id>.cia (~2 MB),
#                                             reading the game files from sdmc:/3ds/3ds-d2/game/; with
#                                             <game-dir> (or D2_GAME_DIR) its icon, banner and tune are the game's
#   scripts/docker.sh package-cia --slim <game-dir>   ... with your 1.14d files inside (no video/music/speech)
#   scripts/docker.sh package-cia --full <game-dir>   ... with every MPQ inside
#     D2_REPORT_DSN=<Sentry DSN> scripts/docker.sh package-cia ...   crash reports offered to that project (issue #26);
#                                             default packaging/cia/report.dsn (scripts/sentry-setup.sh), empty: none
#   scripts/docker.sh game-3dsx          the game app as a .3dsx for Azahar (scripts/run-game-azahar.sh)
#   scripts/docker.sh sdcard-folder <game-dir>   dist/sdcard/3ds/3ds-d2/game/ for that (copy once)
#   scripts/docker.sh repro              two clean release builds must match byte-for-byte
#   scripts/docker.sh shell [hosttest]   interactive shell in a build image
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLCHAIN_IMAGE=3ds-d2/toolchain:local
HOSTTEST_IMAGE=3ds-d2/hosttest:local
CTRTOOLS_IMAGE=3ds-d2/ctrtools:local
BANNER_IMAGE=3ds-d2/banner:local

die() { echo "docker.sh: $*" >&2; exit 1; }

# Git Bash (MSYS) on Windows: no argument rewriting (it turns -w /work and the
# -v specs into Windows paths), and host paths as Docker Desktop takes them
# (C:/...). Elsewhere a host path is passed as it is.
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
    host_path() { cygpath -m "$1"; } ;;
*)  host_path() { printf '%s\n' "$1"; } ;;
esac

# The containers read the tree as it is on disk: a CRLF checkout (Git for
# Windows' core.autocrlf=true, in the submodule, which .gitattributes does not
# reach) breaks the scripts and patches, and changes the build ID.
for f in scripts/package-cia.sh third_party/winx86/build.sh; do
    if [ -f "$ROOT/$f" ] && grep -q $'\r' "$ROOT/$f"; then
        die "$f has CRLF line endings. Check the tree out with LF:
    git config core.autocrlf false; git -C third_party/winx86 config core.autocrlf false
then re-check out the affected files (git rm -r --cached . && git reset --hard, in each)"
    fi
done

ensure_image() { # <target> <tag>
    if ! docker image inspect "$2" >/dev/null 2>&1; then
        docker build --target "$1" -t "$2" "$(host_path "$ROOT")"
    fi
}

# Runs a command in an image with the repository mounted at /work and the host
# user's ownership on everything written.
run_in() { # <image> <cmd...>
    local image="$1"; shift
    local tty=(); [ -t 0 ] && [ -t 1 ] && tty=(-it)
    docker run --rm ${tty[@]+"${tty[@]}"} --network "${RUN_NETWORK:-none}" \
        -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$(host_path "$ROOT"):/work" ${RUN_EXTRA[@]+"${RUN_EXTRA[@]}"} -w /work "$image" "$@"
}

build_id() { run_in "$TOOLCHAIN_IMAGE" sh scripts/build-id.sh; }

# D2_AOT_BUILD=1 (issue #113, docs/aot.md): the AOT spike's recompiler runs on
# the user's Game.exe (mounted read-only) and writes C into build/aot/spike/,
# which the lane, the host cases and package-cia then compile in (AOT=...).
# The output is the user's Game.exe in another form: it stays under build/
# and in their own build. aot_prepare <game-dir> sets AOT_MAKE.
AOT_MAKE=()
# The set compiled: tools/aot/<D2_AOT_SET>.args (spike, the default, or
# coverage: tools/aot/coverage.py's list, issue #115), comment lines left out.
aot_args_file() { echo "$ROOT/tools/aot/${D2_AOT_SET:-spike}.args"; }
aot_args() {
    [ -f "$(aot_args_file)" ] || die "no $(aot_args_file) (D2_AOT_SET)"
    grep -v '^[[:space:]]*#' "$(aot_args_file)"
}
aot_prepare() {
    case "${D2_AOT_BUILD:-}" in
    keep) AOT_MAKE=(AOT=/work/build/aot/spike); return 0 ;;   # what the last D2_AOT_BUILD=1 wrote (lanes side by side)
    1) ;;
    *) return 0 ;;
    esac
    local game; game="$(cd "$1" && pwd)"
    local saved=(${RUN_EXTRA[@]+"${RUN_EXTRA[@]}"})
    RUN_EXTRA=(-v "$(host_path "$game/Game.exe"):/aot/Game.exe:ro")
    # shellcheck disable=SC2046
    run_in "$HOSTTEST_IMAGE" python3 -I tools/aot/recompile.py /aot/Game.exe --out build/aot/spike \
        $(aot_args)
    RUN_EXTRA=(${saved[@]+"${saved[@]}"})
    AOT_MAKE=(AOT=/work/build/aot/spike)
}

do_build() { # <flavor>
    local flavor="$1"
    case "$flavor" in debug|release) ;; *) die "flavor must be debug or release";; esac
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    local id; id="$(build_id)"
    run_in "$TOOLCHAIN_IMAGE" make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
        FLAVOR="$flavor" BUILD_ID="$id"
    echo "built build/$flavor/3ds-d2.3dsx (build_id=$id)"
}

cmd="${1:-}"; shift || true
case "$cmd" in
image)
    docker build --target toolchain -t "$TOOLCHAIN_IMAGE" "$(host_path "$ROOT")"
    docker build --target hosttest -t "$HOSTTEST_IMAGE" "$(host_path "$ROOT")"
    ;;
lock)
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    ensure_image hosttest "$HOSTTEST_IMAGE"
    {
        sed -n 's/^FROM \([^ ]*\) AS \(.*\)$/image \2 \1/p' "$ROOT/Dockerfile"
        echo "platform $(docker image inspect "$TOOLCHAIN_IMAGE" --format '{{.Os}}/{{.Architecture}}')"
        run_in "$TOOLCHAIN_IMAGE" sh -c 'echo "gcc $(arm-none-eabi-gcc -dumpversion)"; dkp-pacman -Q | sed "s/^/dkp /"'
        run_in "$HOSTTEST_IMAGE" sh -c 'dpkg-query -W -f "deb \${Package} \${Version}\n" \
            gcc-arm-linux-gnueabi gcc-14-arm-linux-gnueabi libc6-dev-armel-cross qemu-user make gcc-i686-linux-gnu gcc-14-i686-linux-gnu libc6-dev-i386-cross binutils-i686-linux-gnu python3'
        run_in "$HOSTTEST_IMAGE" python3 -I -c 'from importlib.metadata import version
for m in ("unicorn", "capstone", "pefile"): print("pip", m, version(m))'
    } > "$ROOT/containers/toolchain.lock"
    echo "wrote containers/toolchain.lock"
    ;;
build)
    do_build "${1:-debug}"
    ;;
test)
    ensure_image hosttest "$HOSTTEST_IMAGE"
    case "${1:-host}" in
    host) run_in "$HOSTTEST_IMAGE" make -C tests/host run ;;
    x86)  run_in "$HOSTTEST_IMAGE" make -C tests/x86 "${2:-run}" ;;   # "golden" regenerates
    cpu)  run_in "$HOSTTEST_IMAGE" make -C tests/cpu VARIANT="${2:-armv7}" run ;;
    m3)   run_in "$HOSTTEST_IMAGE" make -C tests/m3 run ;;
    report) run_in "$HOSTTEST_IMAGE" make -C tests/report run ;;   # game.log summary and Sentry envelope
    governor) run_in "$HOSTTEST_IMAGE" make -C tests/governor run ;;   # the sprite cache governor's policy
    gameexe) run_in "$HOSTTEST_IMAGE" make -C tests/gameexe run ;;   # 1.14d's Game.exe told from the others
    frames) run_in "$HOSTTEST_IMAGE" make -C tests/frames run ;;   # the allocator of the sprite cache's frames
    pad)  run_in "$HOSTTEST_IMAGE" make -C tests/pad run ;;   # presses shorter than a frame kept (issue #118)
    pool) run_in "$HOSTTEST_IMAGE" make -C tests/pool run ;;   # where the pool's VirtualAlloc face fits a block without the spare
    d2d)  # [<game-dir>]: d2d's work tree, prepared and patched (issue #106); with the game directory
          # (mounted read-only), d2d's emulator-made cases against our natives (issue #108)
        if [ -n "${2:-}" ]; then
            [ -f "$2/Game.exe" ] || die "test d2d [<game-dir with Game.exe>]"
            aot_prepare "$2"
            RUN_EXTRA=(-v "$(host_path "$(cd "$2" && pwd)"):/game:ro")
            for v in CASES ONLY GEN_CASES_DEBUG; do [ -n "${!v:-}" ] && RUN_EXTRA+=(-e "$v=${!v}"); done
            run_in "$HOSTTEST_IMAGE" make -C tests/d2d run cases ${AOT_MAKE[@]+"${AOT_MAKE[@]}"}
        else
            run_in "$HOSTTEST_IMAGE" make -C tests/d2d run
        fi ;;
    tls)  # the TLS module (tls.bin) loaded as on the console: one test event to Sentry, an untrusted server refused
        ensure_image toolchain "$TOOLCHAIN_IMAGE"
        run_in "$TOOLCHAIN_IMAGE" make -s -C tests/cpu/ctr ../../../build/cpu/ctr/tls.bin
        RUN_NETWORK=bridge run_in "$HOSTTEST_IMAGE" sh tests/tls/run.sh ;;   # the only lane with the network
    m4)   # <game-dir> [guest-ms] [command line]: the game directory is mounted read-only
        [ -n "${2:-}" ] && [ -f "$2/Game.exe" ] || die "test m4 <game-dir with Game.exe> [guest-ms] [command line]"
        aot_prepare "$2"
        make_args=(-C tests/m4 run MS="${3:-20000}" ${AOT_MAKE[@]+"${AOT_MAKE[@]}"})
        [ -n "${M4_OUT:-}" ] && make_args+=(OUT="/work/$M4_OUT")   # e.g. build/m4-b: a lane of its own
        [ -n "${4:-}" ] && make_args+=(CMD="$4")
        RUN_EXTRA=(-v "$(host_path "$(cd "$2" && pwd)"):/game:ro")
        for v in M4_LAYOUT M4_VIRTUAL_CLOCK M4_SCRIPT M4_BOTTOM RT_MSGLOG M4_PHYS_MB M4_FAST M4_QEMU_FLAGS WX86_SON WX86_SONDUMP WX86_SONMAXS WX86_HOTPROF D2_NOLINK M4_FRAMEHASH M4_NATIVE D2_NATIVE_MASK M4_DUMPFRAME M4_VIDEO M4_VIDEO_WAV M4_PROF RT_FILETRACE M4_HASHMEM WX86_FAKETSC M4_ASYNCIO M4_SPRITE_MB M4_POOL_RESERVE_MB M4_POOL_SPARE_KB M4_SPRITE_TRACE M4_STORM_PRIO M4_STORM_DIRECT D2_NATIVE_CHECK M4_SPRITE_FRAMES M4_META_CAP_KB D2_JITEVICT D2_JITFAILAFTER D2_JITCHUNK_KB M4_POOL_PIECES M4_POOL_BALLAST_MB M4_POOL_LARGE_KB M4_POOL_LARGE_MIN_KB M4_BG_QUANTUM M4_CALLRET M4_NOPEND M4_BUDGETTAIL M4_FORWARD M4_HOTPROF_LEVEL M4_HOTPROF_TOP D2_DRAW_CENSUS D2_FLOOR_QUEUE D2_DCC D2_AOT D2_AOT_ONLY WX86_BLOCKLOG; do [ -n "${!v:-}" ] && RUN_EXTRA+=(-e "$v=${!v}"); done
        run_in "$HOSTTEST_IMAGE" make "${make_args[@]}" ;;
    aot)  # <game-dir> [<save-dir>] (issue #114): the recompiler on its Game.exe, then the lane's scripts with
          # D2_AOT=5 in one lane of their own (build/m4-aot), each from a fresh write folder (Act II's a copy
          # of the save) and its log kept as build/m4-aot/<script>.txt; from each, the "aot check:" totals:
          # PASS with calls checked and none differing.
        [ -n "${2:-}" ] && [ -f "$2/Game.exe" ] || die "test aot <game-dir with Game.exe> [<save-dir>]"
        [ -z "${3:-}" ] || [ -f "$3/registry.txt" ] || die "test aot: $3 is not a save folder (no registry.txt)"
        # D2_AOT_BUILD=keep: the C the last run wrote; AOT_RUNS=<script ...>: those scripts alone;
        # D2_AOT_FNS=<first>-<last>: only those hookable functions (src/game/aot_spike.cpp); AOT_MODE=1:
        # the production mode instead of the check (the lane's end alone is judged then); AOT_SPLIT=1:
        # compiled functions calling each other through Box86 (tests/m4/Makefile).
        D2_AOT_BUILD="${D2_AOT_BUILD:-1}" aot_prepare "$2"
        runs=("town-walk 70000" "fight 120000")
        [ -n "${3:-}" ] && runs+=("act2-east 150000")
        if [ -n "${AOT_RUNS:-}" ]; then
            all=("${runs[@]}"); runs=()
            for r in "${all[@]}"; do [[ " $AOT_RUNS " == *" ${r% *} "* ]] && runs+=("$r"); done
        fi
        out=build/m4-aot
        save="${3:-}"
        mkdir -p "$ROOT/$out"
        # One build for every script (AOT_JOBS, 2: two of the generated C's three objects side by side;
        # three at once ran out of Docker's 7.75 GB, cc1 killed, with this branch's C), then each
        # script in a folder of its own (build/m4-aot/<script>/, the binary shared: run-built). The
        # check's scripts run side by side, each qemu a thread of its own (AOT_PARALLEL=0: one at a
        # time); a timed run (AOT_MODE) one at a time unless AOT_PARALLEL=1, so the runs don't share
        # the host's cores. Each run is cut after AOT_TIMEOUT seconds (3600: side by side, Act II's
        # took over 1800 with another lane on the host).
        if ! run_in "$HOSTTEST_IMAGE" make -j"${AOT_JOBS:-2}" -C tests/m4 host OUT="/work/$out" "${AOT_MAKE[@]}" \
                ${AOT_SPLIT:+AOT_SPLIT=1} > "$ROOT/$out/make.txt" 2>&1 < /dev/null; then
            echo "RESULT aot.build FAIL (see $out/make.txt)"
            grep -a "error" "$ROOT/$out/make.txt" | head -10 || true
            exit 1
        fi
        par="${AOT_PARALLEL:-}"
        [ -n "$par" ] || { [ "${AOT_MODE:-5}" = 5 ] && par=1 || par=0; }
        aot_run_one() { # <script> <ms>
            local name="$1" ms="$2" dir="$out/$1"
            rm -rf "${ROOT:?}/$dir" && mkdir -p "$ROOT/$dir/write"
            [ "$name" = act2-east ] && cp -R "$save/." "$ROOT/$dir/write/"
            local RUN_EXTRA=(-v "$game_mount:/game:ro" -e "D2_AOT=${AOT_MODE:-5}" -e "D2_AOT_FNS=${D2_AOT_FNS:-}" -e M4_VIRTUAL_CLOCK=1 -e WX86_FAKETSC=1
                       -e "M4_SCRIPT=$(cat "$ROOT/tests/m4/scripts/$name.txt")")
            for v in ${AOT_ENV:-}; do RUN_EXTRA+=(-e "$v"); done   # AOT_ENV="M4_ASYNCIO=1 D2_DCC=2": the console's ways
            run_in "$HOSTTEST_IMAGE" make -C tests/m4 run-built MS="$ms" OUT="/work/$dir" BIN_OUT="/work/$out" TIMEOUT="${AOT_TIMEOUT:-3600}" \
                "${AOT_MAKE[@]}" ${AOT_SPLIT:+AOT_SPLIT=1} > "$ROOT/$out/$name-make.txt" 2>&1 < /dev/null || true
            cp "$ROOT/$dir/err.txt" "$ROOT/$out/$name.txt" 2>/dev/null || : > "$ROOT/$out/$name.txt"
        }
        game_mount="$(host_path "$(cd "$2" && pwd)")"
        for r in "${runs[@]}"; do
            if [ "$par" = 1 ]; then aot_run_one "${r% *}" "${r#* }" & else aot_run_one "${r% *}" "${r#* }"; fi
        done
        wait
        fail=0
        for r in "${runs[@]}"; do
            name="${r% *}"
            last="$(grep -ah "aot check: .* since the start" "$ROOT/$out/$name.txt" | tail -1 || true)"
            differs="$(grep -ac "aot check: .* differs on its call" "$ROOT/$out/$name.txt" || true)"
            calls="$(sed -n 's/.*since the start \([0-9]*\) calls.*/\1/p' <<<"$last")"
            ndiff="$(sed -n 's/.*functions, \([0-9]*\) differ.*/\1/p' <<<"$last")"
            status="$(sed -n 's/^exit status //p' "$ROOT/$out/$name-make.txt" | tail -1)"
            # The game's own end: a script's run ends at its time (code 0); an exit of the game's
            # (an exception it raised, say) is a failure whatever was checked before it.
            code="$(sed -n 's/^\[rt\] RESULT .*code=\([0-9]*\).*/\1/p' "$ROOT/$out/$name.txt" | tail -1)"
            # The script's own end (issue #115): its last wait reached, else the game played something
            # else (a run that never got in game measures the menus).
            goal="$(sed -En 's/^[0-9]+ (wait|tapuntil) ([a-z0-9]+).*/\2/p' "$ROOT/tests/m4/scripts/$name.txt" | tail -1)"
            if [ -n "$goal" ] && ! grep -aq "autoplay: $goal reached" "$ROOT/$out/$name.txt"; then
                echo "RESULT aot.check.$name FAIL (the script never reached its last wait, $goal; see $out/$name.txt)"
                fail=1
            elif [ "${code:-x}" != 0 ]; then
                echo "RESULT aot.check.$name FAIL (the game ended with code ${code:-none}; ${last:-no aot check line}; see $out/$name.txt)"
                grep -ah "RaiseException\|exception" "$ROOT/$out/$name.txt" | head -3 || true
                fail=1
            elif [ "${AOT_MODE:-5}" != 5 ]; then
                echo "RESULT aot.run.$name PASS (mode ${AOT_MODE}, the game ran to its time)"
            elif [ -n "$calls" ] && [ "$calls" -gt 0 ] && [ "${ndiff:-1}" = 0 ] && [ "${differs:-0}" = 0 ]; then
                echo "RESULT aot.check.$name PASS (lane exit ${status:-?}; since the start ${last#*since the start })"
            else
                echo "RESULT aot.check.$name FAIL (lane exit ${status:-?}; ${last:-no aot check line: see $out/$name.txt})"
                grep -ah "aot check: .* differs" "$ROOT/$out/$name.txt" | head -5 || true
                fail=1
            fi
        done
        exit "$fail" ;;
    aot-explain)  # <game-dir> <va,va,...> (issue #115): why each is left to Box86, with D2_AOT_SET's summaries
        [ -n "${2:-}" ] && [ -f "$2/Game.exe" ] && [ -n "${3:-}" ] || die "test aot-explain <game-dir> <va,...>"
        RUN_EXTRA=(-v "$(host_path "$(cd "$2" && pwd)/Game.exe"):/aot/Game.exe:ro")
        # shellcheck disable=SC2046
        run_in "$HOSTTEST_IMAGE" python3 -I tools/aot/recompile.py /aot/Game.exe --explain "$3" $(aot_args) ;;
    *)    die "test host | test x86 [golden] | test cpu [armv7|armv6] | test m3 | test report | test governor | test gameexe | test frames | test pad | test pool | test d2d [<game-dir>] | test tls | test m4 <game-dir> | test aot <game-dir> [<save-dir>] | test aot-explain <game-dir> <va,...>" ;;
    esac
    ;;
package)
    do_build release
    run_in "$TOOLCHAIN_IMAGE" sh scripts/package.sh release
    ;;
package-cia)   # [<game-dir> | --slim|--full <game-dir>]: the game app (+ the user's own 1.14d files) -> dist/cia/*.cia
    [ "${1:-}" = --prof ] && die "package-cia --prof is gone: every build has the profiler (INFO > profiler run)"
    mode=sdcard
    case "${1:-}" in --slim) mode=slim; shift ;; --full) mode=full; shift ;; --sdcard) shift ;; esac
    stage="$ROOT/build/cia/romfs/game"
    rm -rf "$ROOT/build/cia/romfs" "$ROOT/build/cia/art-src" "$ROOT/build/cia/game-art" && mkdir -p "$stage"
    RUN_EXTRA=(-e "D2_CIA_MODE=$mode")
    [ -n "${D2_BANNER:-}" ] && RUN_EXTRA+=(-e "D2_BANNER=$D2_BANNER")
    [ -n "${D2_BANNER_FRAMES:-}" ] && RUN_EXTRA+=(-e "D2_BANNER_FRAMES=$D2_BANNER_FRAMES")
    [ -n "${D2_BANNER_TURN:-}" ] && RUN_EXTRA+=(-e "D2_BANNER_TURN=$D2_BANNER_TURN")
    # The HOME-menu art (packaging/cia/game_art.py) comes from the user's
    # Game.exe, d2data.mpq, d2exp.mpq and d2xmusic.mpq, mounted read-only outside the
    # RomFS: the CIA holds the icon, banner and tune made from them. (For
    # sdcard that is all that is used of the game directory.)
    art="${1:-${D2_GAME_DIR:-}}"
    if [ -n "$art" ]; then
        [ -f "$art/Game.exe" ] || die "package-cia <game-dir>: no Game.exe in $art"
        art="$(cd "$art" && pwd)"
        for f in "$art"/*; do
            case "$(basename "$f" | tr 'A-Z' 'a-z')" in
            game.exe|d2data.mpq|d2exp.mpq|d2xmusic.mpq) RUN_EXTRA+=(-v "$(host_path "$f"):/work/build/cia/art-src/$(basename "$f"):ro") ;;
            esac
        done
    fi
    if [ "$mode" != sdcard ]; then
        [ -n "${1:-}" ] && [ -f "$1/Game.exe" ] || die "package-cia --$mode <game-dir with Game.exe and the MPQs>"
        game="$(cd "$1" && pwd)"
        # Slim: only what the game reads without audio or cinematics; the rest
        # become empty MPQs (package-cia.sh). Files are bind-mounted one by one
        # (read-only) into the RomFS staging folder: nothing is copied.
        keep='^(game\.exe|d2data\.mpq|d2exp\.mpq|d2char\.mpq|d2sfx\.mpq|patch_d2\.mpq)$'
        for f in "$game"/*; do
            b="$(basename "$f")"
            [ -f "$f" ] || continue
            if [ "$mode" = full ] || echo "$b" | tr 'A-Z' 'a-z' | grep -Eq "$keep"; then
                RUN_EXTRA+=(-v "$(host_path "$f"):/work/build/cia/romfs/game/$b:ro")
            fi
        done
    fi
    ensure_image hosttest "$HOSTTEST_IMAGE"
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    ensure_image ctrtools "$CTRTOOLS_IMAGE"
    saved_extra=("${RUN_EXTRA[@]}"); RUN_EXTRA=()
    run_in "$HOSTTEST_IMAGE" make -C tests/cpu VARIANT=armv6 lib >/dev/null     # the patched backend tree
    if [ "${D2_AOT_BUILD:-}" = 1 ] || [ "${D2_AOT_BUILD:-}" = keep ]; then   # the AOT spike (issue #113): needs the user's Game.exe (keep: the C the last run wrote)
        [ -n "$art" ] || die "D2_AOT_BUILD=$D2_AOT_BUILD package-cia <game-dir>: the recompiler reads its Game.exe"
        aot_prepare "$art"
        echo "docker.sh: this CIA holds code made from your Game.exe: it is yours alone, never share it (docs/aot.md)"
    fi
    # Reports (issue #26): the build ID and version ("0.4.0-dev" unless built
    # from its release tag: scripts/version.sh, issue #61), and the Sentry
    # project if D2_REPORT_DSN is set (even to nothing: no reports); otherwise
    # the Makefile takes packaging/cia/report.dsn.
    version="$(sh "$ROOT/scripts/version.sh")"
    bid="$(build_id)"
    # The build ID covers the AOT step: the recompiler's and its arguments'
    # hash, never Game.exe's.
    [ ${#AOT_MAKE[@]} -gt 0 ] && bid="$bid-aot$(cat "$ROOT/tools/aot/recompile.py" "$(aot_args_file)" | sha256sum | cut -c1-6)"
    report_vars=(BUILD_ID="$bid" D2_VERSION="$version" ${AOT_MAKE[@]+"${AOT_MAKE[@]}"})
    [ -n "${D2_REPORT_DSN+x}" ] && report_vars+=(D2_REPORT_DSN="$D2_REPORT_DSN")
    run_in "$TOOLCHAIN_IMAGE" make -s -j8 -C tests/cpu/ctr "${report_vars[@]}" ../../../build/cpu/ctr/3ds-d2-game-cia.elf ../../../build/cpu/ctr/tls.bin
    RUN_EXTRA=("${saved_extra[@]}" -e "D2_VERSION=$version" -e "D2_BUILD_ID=$bid")   # the CIA's name
    if [ -n "$art" ]; then   # the icon, banner model and tune (packaging/cia/game_art.py)
        ensure_image banner "$BANNER_IMAGE"
        mkdir -p "$ROOT/build/cia/game-art"
        run_in "$BANNER_IMAGE" python3 -I packaging/cia/game_art.py build/cia/art-src build/cia/game-art /opt/pycgfx
    fi
    run_in "$CTRTOOLS_IMAGE" sh scripts/package-cia.sh
    ;;
game-3dsx)   # the game app as a .3dsx for Azahar (scripts/run-game-azahar.sh): build/cpu/ctr/3ds-d2-game.3dsx
    ensure_image hosttest "$HOSTTEST_IMAGE"
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    run_in "$HOSTTEST_IMAGE" make -C tests/cpu VARIANT=armv6 lib >/dev/null     # the patched backend tree
    run_in "$TOOLCHAIN_IMAGE" make -s -j8 -C tests/cpu/ctr BUILD_ID="$(build_id)" D2_VERSION="$(sh "$ROOT/scripts/version.sh")"         ../../../build/cpu/ctr/3ds-d2-game.3dsx
    echo "built build/cpu/ctr/3ds-d2-game.3dsx"
    ;;
sdcard-folder)   # [--full] <game-dir>: dist/sdcard/3ds/3ds-d2/game/ to copy once to the SD card root
    full=0; [ "${1:-}" = --full ] && { full=1; shift; }
    [ -n "${1:-}" ] && [ -f "$1/Game.exe" ] || die "sdcard-folder [--full] <game-dir with Game.exe and the MPQs>"
    game="$(cd "$1" && pwd)"
    dest="$ROOT/dist/sdcard/3ds/3ds-d2/game"
    rm -rf "$ROOT/dist/sdcard" && mkdir -p "$dest"
    # The files the game reads (hard links: nothing is copied on this disk;
    # cp if the game directory is on another volume), and empty MPQs for the
    # cinematics, music and speech it only checks for (as in the slim CIA).
    for f in "$game"/*; do
        b="$(basename "$f")"; l="$(echo "$b" | tr 'A-Z' 'a-z')"
        case "$l" in
        game.exe|d2data.mpq|d2exp.mpq|d2char.mpq|d2sfx.mpq|patch_d2.mpq) ln "$f" "$dest/$b" 2>/dev/null || cp "$f" "$dest/$b" ;;
        *.mpq) [ "$full" = 1 ] && { ln "$f" "$dest/$b" 2>/dev/null || cp "$f" "$dest/$b"; } ;;
        esac
    done
    # Slim: empty MPQs for the archives the game only checks for. Full: the
    # real ones (music, speech and the cinematics).
    if [ "$full" = 0 ]; then
        for f in d2video.mpq d2xvideo.mpq d2music.mpq d2xmusic.mpq d2speech.mpq d2xtalk.mpq; do
            python3 -I "$ROOT/packaging/cia/make_stub_mpq.py" "$dest/$f"
        done
    fi
    ls -la "$dest"; du -sh "$ROOT/dist/sdcard"
    echo "copy dist/sdcard/3ds onto the SD card root (it merges into the existing 3ds folder),"
    echo "then install the CIA from: scripts/docker.sh package-cia"
    ;;
repro)
    ensure_image toolchain "$TOOLCHAIN_IMAGE"
    id="$(build_id)"
    run_in "$TOOLCHAIN_IMAGE" sh scripts/repro.sh "$id"
    ;;
shell)
    if [ "${1:-}" = hosttest ]; then ensure_image hosttest "$HOSTTEST_IMAGE"; run_in "$HOSTTEST_IMAGE" bash
    else ensure_image toolchain "$TOOLCHAIN_IMAGE"; run_in "$TOOLCHAIN_IMAGE" bash; fi
    ;;
*)
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
