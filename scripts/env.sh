# Source this file: `source scripts/env.sh`
# Sets up the PS5 payload SDK and the host LLVM for every build in this repo.

PS5LM_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
export PS5LM_ROOT

export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$PS5LM_ROOT/.deps/ps5-payload-sdk}"
export PS5_HOST="${PS5_HOST:-ps5}"
export PS5_PORT="${PS5_PORT:-9021}"

# The SDK drives the host clang through llvm-config. On macOS, Homebrew's llvm@21
# is the tested one (lld comes from the unversioned `lld` formula, which the SDK
# looks up by itself). On Linux, the SDK finds llvm-config-18..21 on its own.
if [ -z "${LLVM_CONFIG:-}" ] && [ "$(uname -s)" = "Darwin" ]; then
    if [ -x /opt/homebrew/opt/llvm@21/bin/llvm-config ]; then
        export LLVM_CONFIG=/opt/homebrew/opt/llvm@21/bin/llvm-config
    fi
fi

if [ ! -x "$PS5_PAYLOAD_SDK/bin/prospero-clang" ]; then
    echo "env.sh: no SDK at $PS5_PAYLOAD_SDK, run scripts/setup-sdk.sh" >&2
fi
