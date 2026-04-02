#ifndef _WIN32

/*
 * cli_history.c - Persistent command history with arrow-key navigation
 *
 * POSIX-only implementation.
 * Public API is declared in cli_history.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <pwd.h>

#include "cli_history.h"

/* log_verbose is defined in ecat_cli.c */
extern void log_verbose(const char *format, ...);

/* ============================================================================
 * Внутреннее состояние
 * ============================================================================ */

static CommandHistory cmd_history = { .count = 0, .current = -1 };
static char history_filepath[512] = "";

/* ============================================================================
 * Приватные вспомогательные функции
 * ============================================================================ */

/**
 * Получить путь к файлу истории в домашней директории пользователя.
 * Результат сохраняется в history_filepath.
 */
static void history_get_filepath(void) {
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }

    if (home && strlen(home) < 450) {
        snprintf(history_filepath, sizeof(history_filepath),
                 "%s/%s", home, HISTORY_FILENAME);
    } else {
        /* Fallback на /tmp если HOME не доступна */
        snprintf(history_filepath, sizeof(history_filepath),
                 "/tmp/%s", HISTORY_FILENAME);
    }
}

/**
 * Сохранить историю команд в файл (внутренняя функция).
 */
static void history_save(void) {
    if (strlen(history_filepath) == 0) {
        history_get_filepath();
    }

    FILE *f = fopen(history_filepath, "w");
    if (!f) {
        log_verbose("WARNING: Could not open history file for writing: %s",
                    history_filepath);
        return;
    }

    for (int i = 0; i < cmd_history.count; i++) {
        fprintf(f, "%s\n", cmd_history.commands[i]);
    }

    fclose(f);
}

/**
 * Получить команду из истории по смещению от конца.
 * offset=1 -> последняя команда, offset=2 -> предпоследняя и т.д.
 * Возвращает NULL если смещение вне диапазона.
 */
static const char *history_get_back(int offset) {
    if (cmd_history.count == 0 || offset <= 0 || offset > cmd_history.count) {
        return NULL;
    }
    return cmd_history.commands[cmd_history.count - offset];
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

/**
 * Загрузить историю команд из файла (~/.ecat_cli_history).
 */
void history_load(void) {
    if (strlen(history_filepath) == 0) {
        history_get_filepath();
    }

    FILE *f = fopen(history_filepath, "r");
    if (!f) return;  /* Файл ещё не существует — это нормально */

    cmd_history.count = 0;
    while (cmd_history.count < MAX_HISTORY) {
        if (fgets(cmd_history.commands[cmd_history.count],
                  MAX_COMMAND_LEN, f) == NULL) {
            break;
        }

        /* Убираем trailing newline */
        size_t len = strlen(cmd_history.commands[cmd_history.count]);
        if (len > 0 && cmd_history.commands[cmd_history.count][len - 1] == '\n') {
            cmd_history.commands[cmd_history.count][len - 1] = '\0';
        }

        /* Пропускаем пустые строки */
        if (strlen(cmd_history.commands[cmd_history.count]) > 0) {
            cmd_history.count++;
        }
    }

    fclose(f);
    log_verbose("Loaded %d commands from history", cmd_history.count);
}

/**
 * Добавить команду в историю и сохранить в файл.
 */
void history_add(const char *cmd) {
    /* Не добавляем пустые команды */
    if (!cmd || strlen(cmd) == 0) return;

    /* Не добавляем последовательные дубликаты */
    if (cmd_history.count > 0) {
        if (strcmp(cmd_history.commands[cmd_history.count - 1], cmd) == 0) {
            return;
        }
    }

    if (cmd_history.count < MAX_HISTORY) {
        strncpy(cmd_history.commands[cmd_history.count], cmd, MAX_COMMAND_LEN - 1);
        cmd_history.commands[cmd_history.count][MAX_COMMAND_LEN - 1] = '\0';
        cmd_history.count++;
    } else {
        /* Циклический буфер — сдвигаем все записи на одну позицию */
        for (int i = 0; i < MAX_HISTORY - 1; i++) {
            strcpy(cmd_history.commands[i], cmd_history.commands[i + 1]);
        }
        strncpy(cmd_history.commands[MAX_HISTORY - 1], cmd, MAX_COMMAND_LEN - 1);
        cmd_history.commands[MAX_HISTORY - 1][MAX_COMMAND_LEN - 1] = '\0';
    }

    /* Сбросить текущий индекс навигации */
    cmd_history.current = -1;

    /* Сохранить в файл */
    history_save();
}

/**
 * Вывести всю историю команд на stdout с порядковыми номерами.
 */
void history_show(void) {
    if (cmd_history.count == 0) {
        printf("История пуста\n");
        return;
    }
    printf("\nИстория команд (%d):\n", cmd_history.count);
    for (int i = 0; i < cmd_history.count; i++) {
        printf("  %3d: %s\n", i + 1, cmd_history.commands[i]);
    }
    printf("\n");
}

/**
 * Очистить историю в памяти и удалить файл истории на диске.
 */
void history_clear(void) {
    if (strlen(history_filepath) == 0) {
        history_get_filepath();
    }

    cmd_history.count = 0;
    cmd_history.current = -1;

    if (unlink(history_filepath) == 0) {
        printf("История очищена\n");
    }
}

/**
 * Читать строку из stdin с поддержкой навигации по истории (UP/DOWN)
 * и удаления символов (Backspace).
 */
bool read_line_with_history(char *buffer, size_t max_len) {
    struct termios orig_termios, raw_termios;

    /* Сохранить оригинальные настройки терминала */
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        /* Fallback на обычный fgets */
        if (fgets(buffer, max_len, stdin) == NULL) return false;
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
        return true;
    }

    raw_termios = orig_termios;
    /* Отключаем canonical mode и echo */
    raw_termios.c_lflag &= ~(ICANON | ECHO);
    raw_termios.c_cc[VMIN]  = 1;
    raw_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios) == -1) {
        /* Fallback */
        if (fgets(buffer, max_len, stdin) == NULL) return false;
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
        return true;
    }

    int pos = 0;
    int history_offset = 0;  /* 0 = текущая строка, 1 = последняя команда и т.д. */
    char temp_buffer[MAX_COMMAND_LEN];
    strcpy(temp_buffer, "");
    const char *prompt = "CLI> ";

    printf("%s", prompt);
    fflush(stdout);

    while (1) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == -1) {
            break;
        }

        if (c == '\n' || c == '\r') {
            /* Enter — завершить ввод */
            buffer[pos] = '\0';
            printf("\n");
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            return true;
        }
        else if (c == 127 || c == '\b') {
            /* Backspace */
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
                buffer[pos] = '\0';
                strcpy(temp_buffer, buffer);
                history_offset = 0;
            }
        }
        else if (c == 27) {
            /* ESC — возможно начало escape-последовательности */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;

            if (seq[1] == 'A') {
                /* UP — показать предыдущую команду */
                history_offset++;
                const char *hist = history_get_back(history_offset);
                if (hist == NULL) {
                    history_offset--;
                    continue;
                }

                /* Очистить текущую строку и вывести команду из истории */
                printf("\r");
                for (int i = 0; i < pos; i++) printf(" ");
                printf("\r%s%s", prompt, hist);
                strcpy(buffer, hist);
                pos = strlen(hist);
                fflush(stdout);
            }
            else if (seq[1] == 'B') {
                /* DOWN — показать следующую команду или восстановить текущий буфер */
                if (history_offset > 0) {
                    history_offset--;
                    const char *hist;
                    if (history_offset == 0) {
                        hist = temp_buffer;
                    } else {
                        hist = history_get_back(history_offset);
                    }

                    if (hist == NULL) hist = "";

                    printf("\r");
                    for (int i = 0; i < pos; i++) printf(" ");
                    printf("\r%s%s", prompt, hist);
                    strcpy(buffer, hist);
                    pos = strlen(hist);
                    fflush(stdout);
                }
            }
        }
        else if (c >= 32 && c < 127) {
            /* Обычный печатаемый символ */
            if (pos < (int)max_len - 1) {
                buffer[pos] = c;
                pos++;
                buffer[pos] = '\0';
                printf("%c", c);
                fflush(stdout);

                /* Сохранить текущий буфер при ручном вводе */
                strcpy(temp_buffer, buffer);
                history_offset = 0;
            }
        }
    }

    /* Восстановить настройки терминала */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    return false;
}

#endif /* _WIN32 */