#!/bin/bash
# Legacy entrypoint wrapper for Exynos 2100 kernel build.
# The canonical build script is ./build.sh (options: -m <model> [-k y/N] [-s y/N]).
#
# Supported models and their config mappings:
#   o1s: Samsung Galaxy S21 (SM-G991B)       -> exynos2100_defconfig + o1s.config
#   t2s: Samsung Galaxy S21+ 5G (SM-G996B)   -> exynos2100_defconfig + t2s.config
#   p3s: Samsung Galaxy S21 Ultra (SM-G998B) -> exynos2100_defconfig + p3s.config
#   r9s: Samsung Galaxy S21 FE 5G (SM-G990B) -> exynos2100_defconfig + r9s.config

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODEL="${1:-o1s}"
echo "==> Invoking canonical build entrypoint: $SCRIPT_DIR/build.sh -m $MODEL"
exec "$SCRIPT_DIR/build.sh" -m "$MODEL" "${@:2}"
