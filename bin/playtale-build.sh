#!/usr/bin/env bash
set -Eeuo pipefail

# Windows-проект и его Linux-копия
SOURCE="/mnt/e/Projects/CPGamebookPlayer"
DEST="$HOME/projects/CPGamebookPlayer"

# Настройки
ENV_NAME="simulator_x4_pro"
PLAYTALE_DIR="$DEST/playtale"

# Лог
LOG="$PLAYTALE_DIR/build-playtale.log"

echo "======================================"
echo " PlayTale: sync, build and run"
echo "======================================"

# Проверяем исходный каталог
if [[ ! -d "$SOURCE" ]]; then
    echo "ERROR: Source directory not found: $SOURCE"
    exit 1
fi

mkdir -p "$DEST"
mkdir -p "$(dirname "$LOG")"

# 1. Синхронизация
echo
echo "[1/2] Synchronizing project..."

rsync -av --delete \
    --exclude='.git/' \
    --exclude='.pio/' \
    --exclude='.cache/' \
    --exclude='/playtale/build-playtale.log' \
    "$SOURCE/" "$DEST/"

echo "Synchronization completed."

# 2. Сборка и запуск симулятора
echo
echo "[2/2] Building and running $ENV_NAME..."
echo "Build log: $LOG"

cd "$PLAYTALE_DIR"

set +e
pio run -e "$ENV_NAME" -t run_simulator 2>&1 | tee "$LOG"
RESULT=${PIPESTATUS[0]}
set -e

# Копируем лог обратно в Windows-проект
cp "$LOG" "$SOURCE/playtale/build-playtale.log"

if [[ $RESULT -ne 0 ]]; then
    echo
    echo "BUILD/RUN FAILED (exit code: $RESULT)"
    echo "Log copied to: $SOURCE/playtale/build-playtale.log"
    exit "$RESULT"
fi

echo
echo "Simulator finished successfully."
echo "Log copied to: $SOURCE/playtale/build-playtale.log"