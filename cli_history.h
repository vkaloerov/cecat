#ifndef CLI_HISTORY_H
#define CLI_HISTORY_H

/*
 * cli_history.h - Command history API for CLI
 *
 * Provides persistent command history with UP/DOWN arrow navigation.
 * POSIX-only implementation (Linux, macOS, Raspberry Pi OS, etc.)
 */

#include <stddef.h>   /* size_t */
#include <stdbool.h>  /* bool   */

/* ============================================================================
 * Константы
 * ============================================================================ */

#ifndef MAX_COMMAND_LEN
#define MAX_COMMAND_LEN 256
#endif

#define MAX_HISTORY      100
#define HISTORY_FILENAME ".ecat_cli_history"

/* ============================================================================
 * Типы данных
 * ============================================================================ */

typedef struct {
    char commands[MAX_HISTORY][MAX_COMMAND_LEN];
    int count;    /* Количество команд в истории               */
    int current;  /* Текущий индекс при навигации стрелками    */
} CommandHistory;

/* ============================================================================
 * Публичный API (только POSIX)
 * ============================================================================ */

#ifndef _WIN32

/**
 * Загрузить историю команд из файла (~/.ecat_cli_history).
 * Вызывать один раз при запуске REPL.
 */
void history_load(void);

/**
 * Добавить команду в историю и немедленно сохранить в файл.
 * Пустые строки и последовательные дубликаты игнорируются.
 *
 * @param cmd  Команда для добавления (не NULL).
 */
void history_add(const char *cmd);

/**
 * Вывести всю историю команд на stdout с порядковыми номерами.
 */
void history_show(void);

/**
 * Очистить историю в памяти и удалить файл истории на диске.
 */
void history_clear(void);

/**
 * Прочитать строку из stdin с поддержкой навигации по истории
 * через клавиши UP / DOWN и удаления через Backspace.
 *
 * Переводит терминал в raw-режим на время ввода и восстанавливает
 * исходные настройки после завершения.
 *
 * @param buffer   Буфер для записи результирующей строки.
 * @param max_len  Размер буфера в байтах.
 * @return true  — строка успешно прочитана (пользователь нажал Enter).
 * @return false — EOF или ошибка чтения.
 */
bool read_line_with_history(char *buffer, size_t max_len);

#endif /* _WIN32 */

#endif /* CLI_HISTORY_H */