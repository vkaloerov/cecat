/*
 * my_hex_dump.c - Реализация красивого вывода hex данных
 *
 * Функции для форматирования и вывода бинарных данных в читаемом формате
 * аналогично модулю hexdump в Python
 */


#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "my_hex_dump.h"

unsigned int out_copy_chars(char *output, size_t output_len, const char *data, int current_pos, size_t data_size, int shift) {
    size_t to_copy = shift < output_len ? shift : output_len;
    int old_current_pos = current_pos;
    for (size_t i = 0; i < output_len; ++i) {
        output[i] = data[current_pos];
        current_pos++;
    }
    current_pos = old_current_pos + to_copy;
    if ((size_t)current_pos >= data_size) {
        current_pos = 0;
    }

    return current_pos;
}

/**
 * Форматировать одну строку hex dump в буфер
 *
 * Формат:
 * 00000000: 48 65 6c 6c 6f 20 57 6f 72 6c 64 00 2e 2e 2e 2e  |Hello World....|
 *
 * @param output    - Выходной буфер
 * @param output_len - Размер выходного буфера
 * @param offset    - Текущий offset в файле
 * @param data      - Указатель на данные для этой строки
 * @param line_len  - Реальное количество байт в этой строке
 * @param width     - Максимальная ширина строки
 *
 * @return Количество символов, записанных в output
 */
size_t hex_dump_format_line(char *output, size_t output_len,
                            size_t offset, const uint8_t *data,
                            size_t line_len, int width) {
    if (!output || output_len < 80 || !data) {
        return 0;
    }

    char *pos = output;
    size_t remaining = output_len;
    int written = 0;

    /* Адрес (offset) */
    written = snprintf(pos, remaining, "%08lx: ", (unsigned long)offset);
    if (written < 0) return 0;
    pos += written;
    remaining -= written;

    /* Hex представление */
    for (int i = 0; i < width; i++) {
        if (i < (int)line_len) {
            written = snprintf(pos, remaining, "%02x ", data[i]);
        } else {
            /* Пустые байты для выравнивания */
            written = snprintf(pos, remaining, "   ");
        }
        if (written < 0) return 0;
        pos += written;
        remaining -= written;

        /* Группировка: пробел каждые 8 байт */
        if ((i + 1) % 8 == 0 && i + 1 < width) {
            written = snprintf(pos, remaining, " ");
            if (written < 0) return 0;
            pos += written;
            remaining -= written;
        }
    }

    /* Разделитель между hex и ASCII */
    written = snprintf(pos, remaining, " |");
    if (written < 0) return 0;
    pos += written;
    remaining -= written;

    /* ASCII представление */
    for (int i = 0; i < line_len; i++) {
        unsigned char c = data[i];
        if (isprint(c)) {
            written = snprintf(pos, remaining, "%c", c);
        } else {
            written = snprintf(pos, remaining, ".");
        }
        if (written < 0) return 0;
        pos += written;
        remaining -= written;
    }

    /* Закрывающий разделитель */
    written = snprintf(pos, remaining, "|");
    if (written < 0) return 0;
    pos += written;
    remaining -= written;

    return pos - output;
}

/**
 * Вывести hex dump данных в stdout
 *
 * @param data   - Указатель на буфер данных
 * @param length - Длина буфера в байтах
 * @param label  - Опциональная метка (может быть NULL)
 */
void hex_dump_print(const uint8_t *data, size_t length, const char *label) {
    hex_dump_print_width(data, length, 16, label);
}

/**
 * Вывести hex dump данных с указанием ширины строк
 *
 * @param data   - Указатель на буфер данных
 * @param length - Длина буфера в байтах
 * @param width  - Количество байт на строку
 * @param label  - Опциональная метка (может быть NULL)
 */
void hex_dump_print_width(const uint8_t *data, size_t length, int width, const char *label) {
    if (!data || length == 0 || width <= 0) {
        return;
    }

    /* Вывести метку если задана */
    if (label) {
        printf("%s (%zu bytes):\n", label, length);
    }

    /* Вывести каждую строку */
    char line_buffer[256];
    for (size_t offset = 0; offset < length; offset += (size_t)width) {
        size_t remaining = length - offset;
        size_t line_len = (remaining < (size_t)width) ? remaining : width;

        hex_dump_format_line(line_buffer, sizeof(line_buffer),
                             offset, &data[offset],
                             line_len, width);
        printf("%s\n", line_buffer);
    }
}
