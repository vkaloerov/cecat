/*
 * my_hex_dump.h - Красивый вывод hex данных (как hexdump в Python)
 *
 * Функция для форматирования и вывода бинарных данных в читаемом формате:
 * - Offset (адрес)
 * - Hex представление (16 байт на строку)
 * - ASCII представление
 *
 * Пример вывода:
 * 00000000: 48 65 6c 6c 6f 20 57 6f 72 6c 64 00 2e 2e 2e 2e  Hello World....
 * 00000010: 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f 10  ................
 */

#ifndef MY_HEX_DUMP_H
#define MY_HEX_DUMP_H

#include <stdint.h>
#include <stddef.h>

unsigned int out_copy_chars(char *output, size_t output_len, const char *data, int current_pos, size_t data_size, int shift);

/**
 * Вывести hex dump данных в stdout
 *
 * @param data      - Указатель на буфер данных
 * @param length    - Длина буфера в байтах
 * @param label     - Опциональная метка (может быть NULL)
 *
 * Формат вывода:
 *   <label> (если указана):
 *   00000000: xx xx xx xx xx xx xx xx xx xx xx xx xx xx xx xx  |ASCII представление|
 *   00000010: xx xx xx xx xx xx xx xx xx xx xx xx xx xx xx xx  |ASCII представление|
 *   ...
 *
 * Пример использования:
 *   uint8_t data[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f};
 *   hex_dump_print(data, sizeof(data), "My Data");
 *
 *   Вывод:
 *   My Data (5 bytes):
 *   00000000: 48 65 6c 6c 6f                                   Hello
 */
void hex_dump_print(const uint8_t *data, size_t length, const char *label);

/**
 * Вывести hex dump данных с указанием ширины строк
 *
 * @param data      - Указатель на буфер данных
 * @param length    - Длина буфера в байтах
 * @param width     - Количество байт на строку (рекомендуется 16)
 * @param label     - Опциональная метка (может быть NULL)
 *
 * Пример:
 *   hex_dump_print_width(data, 32, 8, "Config");
 *   - Покажет по 8 байт на строку вместо стандартных 16
 */
void hex_dump_print_width(const uint8_t *data, size_t length, int width, const char *label);

/**
 * Форматировать одну строку hex dump в буфер
 *
 * @param output    - Выходной буфер (минимум 80 символов)
 * @param offset    - Текущий offset (для вывода адреса)
 * @param data      - Указатель на строку данных
 * @param line_len  - Количество байт в этой строке (может быть < width)
 * @param width     - Максимальная ширина строки
 *
 * Вернет длину строки (не считая нулевого терминатора)
 */
size_t hex_dump_format_line(char *output, size_t output_len,
                            size_t offset, const uint8_t *data,
                            size_t line_len, int width);

#endif /* MY_HEX_DUMP_H */
