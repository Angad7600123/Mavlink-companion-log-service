# Shared cmake/ninja checks, generator migration, and build progress for install.sh / update.sh.

: "${PROJECT_ROOT:?PROJECT_ROOT must be set}"
: "${BUILD_DIR:?BUILD_DIR must be set}"

BUILD_PATH="${PROJECT_ROOT}/${BUILD_DIR}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
CMAKE_MIN_VERSION="${CMAKE_MIN_VERSION:-3.16}"

require_cmd() {
    local name="$1"
    local pkg="$2"
    if ! command -v "${name}" >/dev/null 2>&1; then
        echo "ERROR: ${name} not found. Install with: sudo apt install ${pkg}" >&2
        exit 1
    fi
}

require_build_tools() {
    require_cmd cmake cmake
    require_cmd ninja ninja-build

    local cmake_version
    cmake_version="$(cmake --version | awk '/^cmake version / { print $3; exit }')"
    if ! printf '%s\n%s\n' "${CMAKE_MIN_VERSION}" "${cmake_version}" | sort -V -C 2>/dev/null; then
        echo "ERROR: cmake ${cmake_version} is too old; need ${CMAKE_MIN_VERSION} or newer." >&2
        exit 1
    fi
}

ensure_cmake_generator() {
    local cache="${BUILD_PATH}/CMakeCache.txt"
    [[ -f "${cache}" ]] || return 0

    local cached
    cached="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${cache}")"
    [[ -n "${cached}" && "${cached}" != "${CMAKE_GENERATOR}" ]] || return 0

    echo "==> Existing build uses '${cached}'; recreating ${BUILD_DIR}/ for ${CMAKE_GENERATOR}"

    local deps_backup=""
    if [[ -d "${BUILD_PATH}/_deps" ]]; then
        deps_backup="$(mktemp -d)"
        mv "${BUILD_PATH}/_deps" "${deps_backup}/"
    fi

    rm -rf "${BUILD_PATH}"
    mkdir -p "${BUILD_PATH}"

    if [[ -n "${deps_backup}" && -d "${deps_backup}/_deps" ]]; then
        mv "${deps_backup}/_deps" "${BUILD_PATH}/"
        rmdir "${deps_backup}" 2>/dev/null || true
    fi
}

mcls_cmake_configure() {
    ensure_cmake_generator
    cmake -S "${PROJECT_ROOT}" -B "${BUILD_PATH}" -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
}

render_build_progress() {
    local current="$1"
    local total="$2"
    local width=30
    local pct=0
    local filled=0
    local bar=""

    [[ "${total}" -gt 0 ]] || return 0
    pct=$((current * 100 / total))
    filled=$((current * width / total))

    bar="$(printf '%*s' "${filled}" '' | tr ' ' '#')"
    bar="${bar}$(printf '%*s' "$((width - filled))" '' | tr ' ' '-')"
    printf '\rBuild [%s] %3d%% (%d/%d)' "${bar}" "${pct}" "${current}" "${total}" >&2
}

# Run ninja with an optional job limit. Set MCLS_BUILD_PROGRESS=0 to disable the bar.
# Usage: mcls_cmake_build [parallel_jobs]
mcls_cmake_build() {
    local jobs="${1:-}"
    local -a ninja_args=(-C "${BUILD_PATH}")
    [[ -n "${jobs}" ]] && ninja_args+=(-j "${jobs}")

    if [[ "${MCLS_BUILD_PROGRESS:-1}" == "0" ]]; then
        ninja "${ninja_args[@]}"
        return $?
    fi

    # Stream ninja through the progress filter while still observing its REAL
    # exit status. ninja runs in a process substitution; capture its PID and
    # wait on it.
    #
    # The previous form `while ... done < <(ninja ...)` then read
    # `exit_code=${PIPESTATUS[0]}`. But PIPESTATUS there reflects the `while`
    # compound command, whose status is the final `read` that hit EOF -- always
    # 1. So the function returned 1 on EVERY successful build, tripping the
    # caller's `set -e` and aborting update.sh/install.sh before the install
    # step. ninja's own exit status was never inspected.
    local line exit_code=0
    exec 3< <(ninja "${ninja_args[@]}" 2>&1)
    local ninja_pid=$!
    while IFS= read -r line <&3; do
        if [[ "${line}" =~ ^\[([0-9]+)/([0-9]+)\] ]]; then
            render_build_progress "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
            printf '\n%s\n' "${line}"
        else
            printf '%s\n' "${line}"
        fi
    done
    exec 3<&-
    wait "${ninja_pid}" || exit_code=$?

    printf '\n' >&2
    return "${exit_code}"
}

# Confirm the freshly built binary was actually installed at the destination:
# it must exist, be executable, and match the build byte-for-byte. Returns
# non-zero (so callers under `set -e` abort) if any check fails.
# Usage: mcls_verify_installed_binary <built_path> <installed_path>
mcls_verify_installed_binary() {
    local built="$1"
    local installed="$2"

    if ! sudo test -e "${installed}"; then
        echo "ERROR: ${installed} does not exist after install." >&2
        return 1
    fi
    if ! sudo test -x "${installed}"; then
        echo "ERROR: ${installed} exists but is not executable after install." >&2
        return 1
    fi

    local built_sum installed_sum
    built_sum="$(sha256sum "${built}" | awk '{print $1}')"
    installed_sum="$(sudo sha256sum "${installed}" | awk '{print $1}')"
    if [[ "${built_sum}" != "${installed_sum}" ]]; then
        echo "ERROR: ${installed} (sha256 ${installed_sum}) does not match the" >&2
        echo "       freshly built ${built} (sha256 ${built_sum}); install did not take effect." >&2
        return 1
    fi

    echo "    OK: ${installed} exists, is executable, and matches the build (sha256 ${built_sum})"
}
