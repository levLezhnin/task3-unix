#!/bin/bash

LOG_FILE="/tmp/myinit.log"
RESULT_FILE="result.txt"
WORK_DIR="/tmp/myinit_test_$$"
MYINIT_PID=""

# Сохраняем абсолютный путь к директории проекта ДО cd
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Функция логирования в result.txt
log_result() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$RESULT_FILE"
}

# Функция очистки
cleanup() {
    log_result "=== Очистка запущена ==="
    if [ -n "$MYINIT_PID" ] && kill -0 "$MYINIT_PID" 2>/dev/null; then
            kill -TERM "$MYINIT_PID" 2>/dev/null || true
            wait "$MYINIT_PID" 2>/dev/null || true
            log_result "myinit завершён (pid $MYINIT_PID)"
    fi
    rm -rf "$WORK_DIR"
    log_result "Очистка завершена"
}

# Инициализация
echo "=== Создание тестового окружения myinit ===" > "$RESULT_FILE"
log_result "Рабочая директория: $WORK_DIR"
log_result "Директория с файлами скриптов: $SCRIPT_DIR"
mkdir -p "$WORK_DIR"

log_result "=== Сборка myinit ==="
make -C "$SCRIPT_DIR" clean >/dev/null 2>&1 || true
if make -C "$SCRIPT_DIR" >> "$RESULT_FILE" 2>&1; then
    log_result "Статус сборки: Успешно"
else
    log_result "Статус сборки: Ошибка"
    echo "Ошибка сборки. Проверь Makefile и myinit.c"
    exit 1
fi

log_result "=== Создание тестовых программ ==="
mkdir -p "$WORK_DIR/test_programs"

for i in 1 2 3; do
    cat > "$WORK_DIR/test_programs/test${i}.c" << EOF
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
int main() {
    printf("Запущена тестовая программа, pid=%d\n", getpid());
    fflush(stdout);
    int total_seconds = 0;
    int delta = 5;
    while (total_seconds < 60) { sleep(delta); total_seconds += delta; printf("Я жив\n"); fflush(stdout); }
    return 0;
}
EOF
    gcc -o "$WORK_DIR/test_programs/test${i}" "$WORK_DIR/test_programs/test${i}.c" 2>> "$RESULT_FILE"
    log_result "Создан файл: test_programs/test${i}"
done

# Создание конфигов
log_result "=== Создание конфигурационных файлов ==="

cat > "$WORK_DIR/config_test1.conf" << EOF
$WORK_DIR/test_programs/test1 $WORK_DIR/in1 $WORK_DIR/out1
$WORK_DIR/test_programs/test2 $WORK_DIR/in2 $WORK_DIR/out2
$WORK_DIR/test_programs/test3 $WORK_DIR/in3 $WORK_DIR/out3
EOF
log_result "Создан файл: config_test1.conf"

cat > "$WORK_DIR/config_test2.conf" << EOF
$WORK_DIR/test_programs/test1 $WORK_DIR/in1 $WORK_DIR/out1
EOF
log_result "Создан файл: config_test2.conf"

# Создание файлов для stdin
touch "$WORK_DIR/in1" "$WORK_DIR/in2" "$WORK_DIR/in3"

# Запуск myinit — используем $SCRIPT_DIR/myinit
log_result "=== Запуск myinit ==="

# 1. Гарантированно убиваем старые демоны
pkill -x myinit 2>/dev/null || true
sleep 1
rm -f /tmp/myinit.log

# 2. Запускаем новый
"$SCRIPT_DIR/myinit" "$WORK_DIR/config_test1.conf" & sleep 2

# 3. Берём PID последнего запущенного myinit
MYINIT_PID=$(pgrep -x myinit | tail -n 1)
if [ -z "$MYINIT_PID" ]; then
    log_result "❌ myinit не запустился. Проверьте /tmp/myinit.log"
    cat /tmp/myinit.log >> "$RESULT_FILE" 2>&1 || true
    exit 1
fi

# Тест 1: Проверка 3 процессов
log_result "=== Тест 1: Запуск 3 процессов ==="
sleep 1

CHILD_COUNT=$(pgrep -P "$MYINIT_PID" 2>/dev/null | wc -l)
if [ "$CHILD_COUNT" -eq 3 ]; then
    log_result "Статус теста 1: Успешно - запущено 3 дочерних процесса"
else
    log_result "Статус теста 1: Провалено -  ожидалось 3 запущенных процесса, было $CHILD_COUNT"
    ps --ppid "$MYINIT_PID" -f >> "$RESULT_FILE" 2>&1 || true
fi

# Тест 2: Убийство и рестарт
log_result "=== Тест 2: Kill 2-ого дочернего процесса и проверка его перезапуска ==="
# Берём второй PID из списка прямых потомков
CHILD2_PID=$(pgrep -P "$MYINIT_PID" 2>/dev/null | sed -n '2p')
if [ -n "$CHILD2_PID" ]; then
    log_result "Kill дочернего процесса 2 с pid=$CHILD2_PID"
    kill -9 "$CHILD2_PID" 2>/dev/null || true
    sleep 2
    NEW_CHILD_COUNT=$(pgrep -P "$MYINIT_PID" 2>/dev/null | wc -l)
    if [ "$NEW_CHILD_COUNT" -eq 3 ]; then
        log_result "Статус теста 2: Успешно - дочерний процесс 2 перезапущен"
    else
        log_result "Статус теста 2: Провалено - ожидалось 3 дочерних процесса, было $NEW_CHILD_COUNT"
    fi
else
    log_result "Статус теста 2: Провалено - дочерний процесс 2 не найден"
fi

# Тест 3: SIGHUP
log_result "=== Тест 3: SIGHUP перезагрузка конфига ==="
cp "$WORK_DIR/config_test2.conf" "$WORK_DIR/config_test1.conf"
kill -HUP "$MYINIT_PID"
sleep 3

FINAL_CHILD_COUNT=$(pgrep -P "$MYINIT_PID" 2>/dev/null | wc -l)
if [ "$FINAL_CHILD_COUNT" -eq 1 ]; then
    log_result "Статус теста 3: Успешно - 1 дочерний процесс после SIGHUP"
else
    log_result "Тест 3: Провалено - ожидался 1 дочерний процесс, было $FINAL_CHILD_COUNT"
fi

# Проверка лога
log_result "=== Проверка логов myinit ==="
if [ -f "$LOG_FILE" ]; then
    log_result "Лог найден"
    grep -E "(запущен|убит|сигналом|Перезапускаю|SIGHUP|конфигурацию)" "$LOG_FILE" >> "$RESULT_FILE" 2>&1 || true
else
    log_result "Статус проверки лога: Провалено - лог не найден"
fi

log_result "=== Тесты завершены ==="
echo ""
echo "Результаты сохранены в файл $RESULT_FILE"

cleanup

cat "$RESULT_FILE"
