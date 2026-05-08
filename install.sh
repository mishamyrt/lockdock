#!/bin/sh
set -eu

REPOSITORY="mishamyrt/lockdock"
API_URL="https://api.github.com/repos/${REPOSITORY}/releases/latest"
INSTALL_DIR="${HOME}/.local/bin"
INSTALL_PATH="${INSTALL_DIR}/lockdock"
SERVICE_MODE="prompt"

die() {
    printf 'Error: %s\n' "$1" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: install.sh [--enable-service | --skip-service]

Install the latest lockdock release from GitHub.

Options:
  --enable-service  Install and start the LaunchAgent without prompting
  --skip-service    Install without starting the LaunchAgent
  --help            Show this help message
EOF
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

set_service_mode() {
    next_mode="$1"

    if [ "$SERVICE_MODE" != "prompt" ] && [ "$SERVICE_MODE" != "$next_mode" ]; then
        die "Choose only one of --enable-service or --skip-service"
    fi

    SERVICE_MODE="$next_mode"
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --enable-service)
                set_service_mode "enable"
                ;;
            --skip-service)
                set_service_mode "skip"
                ;;
            --help|-h)
                usage
                exit 0
                ;;
            *)
                die "Unknown option: $1"
                ;;
        esac

        shift
    done
}

detect_asset_arch() {
    os_name="$(uname -s)"
    machine_arch="$(uname -m)"

    if [ "$os_name" != "Darwin" ]; then
        die "Unsupported operating system: $os_name"
    fi

    case "$machine_arch" in
        arm64|aarch64)
            printf 'arm64\n'
            ;;
        x86_64|amd64)
            printf 'amd64\n'
            ;;
        *)
            die "Unsupported macOS architecture: $machine_arch"
            ;;
    esac
}

fetch_latest_tag() {
    release_json="$(curl -fsSL "$API_URL")"
    tag="$(printf '%s\n' "$release_json" | awk -F'"' '/"tag_name"[[:space:]]*:/ { print $4; exit }')"

    [ -n "$tag" ] || die "Failed to determine the latest release tag"

    printf '%s\n' "$tag"
}

get_installed_version() {
    if [ -x "$INSTALL_PATH" ]; then
        if version_output="$("$INSTALL_PATH" version 2>/dev/null)"; then
            printf '%s\n' "$version_output" | awk '/^lockdock / { print $2; exit }'
        fi
    fi

    return 0
}

download_release() {
    asset_arch="$1"
    tag="$2"
    work_dir="$3"

    archive_name="lockdock_${tag}_darwin_${asset_arch}.tar.gz"
    archive_path="${work_dir}/${archive_name}"
    checksums_path="${work_dir}/sha256sums.txt"
    release_base_url="https://github.com/${REPOSITORY}/releases/download/${tag}"

    printf 'Downloading %s\n' "$archive_name"
    curl -fsSL "${release_base_url}/${archive_name}" -o "$archive_path"
    curl -fsSL "${release_base_url}/sha256sums.txt" -o "$checksums_path"

    expected_sha="$(awk -v archive="$archive_name" '$2 == archive { print $1; exit }' "$checksums_path")"
    [ -n "$expected_sha" ] || die "Checksum for ${archive_name} was not found"

    actual_sha="$(shasum -a 256 "$archive_path" | awk '{ print $1 }')"
    [ "$actual_sha" = "$expected_sha" ] || die "Checksum verification failed for ${archive_name}"

    tar -xzf "$archive_path" -C "$work_dir"

    [ -f "${work_dir}/lockdock" ] || die "Release archive did not contain the lockdock binary"
}

prompt_start_service() {
    answer=""
    prompt="Start the background service now? [Y/n] "

    if [ -t 0 ]; then
        printf '%s' "$prompt" >&2
        IFS= read -r answer || answer=""
    elif [ -t 2 ] && [ -c /dev/tty ]; then
        printf '%s' "$prompt" >/dev/tty
        IFS= read -r answer </dev/tty || answer=""
    else
        printf '%s' "$prompt" >&2
        IFS= read -r answer || answer=""
    fi

    case "$answer" in
        "")
            return 0
            ;;
        y|Y|yes|YES|Yes)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

start_service() {
    printf 'Starting lockdock LaunchAgent\n'
    "$INSTALL_PATH" enable
}

main() {
    parse_args "$@"

    require_command awk
    require_command curl
    require_command mktemp
    require_command shasum
    require_command tar
    require_command uname

    asset_arch="$(detect_asset_arch)"
    tag="$(fetch_latest_tag)"
    latest_version="${tag#v}"
    installed_version="$(get_installed_version)"

    if [ -n "$installed_version" ] && [ "$installed_version" = "$latest_version" ]; then
        printf 'lockdock %s is already installed at %s\n' "$installed_version" "$INSTALL_PATH"
    else
        work_dir="$(mktemp -d "${TMPDIR:-/tmp}/lockdock-install.XXXXXX")"

        trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

        if [ -n "$installed_version" ]; then
            printf 'Updating lockdock from %s to %s\n' "$installed_version" "$latest_version"
        else
            printf 'Installing lockdock %s\n' "$latest_version"
        fi

        download_release "$asset_arch" "$tag" "$work_dir"

        mkdir -p "$INSTALL_DIR"
        cp "${work_dir}/lockdock" "$INSTALL_PATH"
        chmod 755 "$INSTALL_PATH"

        printf 'Installed lockdock to %s\n' "$INSTALL_PATH"
    fi

    case ":$PATH:" in
        *":$INSTALL_DIR:"*)
            ;;
        *)
            printf 'Add %s to PATH if lockdock is not found in new shells.\n' "$INSTALL_DIR"
            ;;
    esac

    printf 'Accessibility permission is required for lockdock to manage the Dock position.\n'

    case "$SERVICE_MODE" in
        enable)
            start_service
            ;;
        skip)
            printf 'Skipping background service startup.\n'
            ;;
        prompt)
            if prompt_start_service; then
                start_service
            else
                printf 'Skipping background service startup.\n'
            fi
            ;;
    esac
}

main "$@"
