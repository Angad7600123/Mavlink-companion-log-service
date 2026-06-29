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

    local line exit_code=0
    while IFS= read -r line; do
        if [[ "${line}" =~ ^\[([0-9]+)/([0-9]+)\] ]]; then
            render_build_progress "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}"
            printf '\n%s\n' "${line}"
        else
            printf '%s\n' "${line}"
        fi
    done < <(ninja "${ninja_args[@]}" 2>&1)
    exit_code=${PIPESTATUS[0]}

    printf '\n' >&2
    return "${exit_code}"
}
