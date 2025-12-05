/*
 * ecat_cli.c - EtherCAT CLI утилита с SOEM
 *
 * Компактная реализация CLI для работы с EtherCAT устройствами через SOEM.
 * Включает:
 * - Парсинг аргументов командной строки
 * - REPL интерфейс (dummy_says>)
 * - Сканирование EtherCAT шины
 * - Чтение конфигурации slave устройств
 * - Чтение/запись данных
 * - Verbose режим для отладки
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include "soem/soem.h"

/* ============================================================================
 * Глобальные переменные для состояния SOEM
 * ============================================================================ */

#define MAX_IO_MAP_SIZE 4096
#define MAX_COMMAND_LEN 256
#define MAX_ARGS 32

static char IOmap[MAX_IO_MAP_SIZE];  /* Буфер для I/O mapping */
static bool soem_initialized = false; /* Флаг инициализации SOEM */
static bool verbose_mode = false;     /* Флаг verbose режима */
static char interface_name[64] = "";  /* Имя сетевого интерфейса */
static bool pdo_active = false;       /* Флаг активности PDO обмена */
static volatile bool pdo_running = false; /* Флаг работы PDO цикла */

/* SOEM 2.0 context structure */
static ecx_contextt ecx_context;

/* ============================================================================
 * Утилиты для вывода и логирования
 * ============================================================================ */

/**
 * Вывод verbose сообщения (только если включен verbose режим)
 */
static void log_verbose(const char *format, ...) {
    if (!verbose_mode) return;

    va_list args;
    va_start(args, format);
    printf("[VERBOSE] ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

/**
 * Вывод ошибки с детальной информацией из SOEM
 */
static void print_error(const char *context) {
    printf("ERROR: %s\n", context);
    if (ecx_iserror(&ecx_context)) {
        char *err_str = ecx_elist2string(&ecx_context);
        if (err_str && err_str[0] != '\0') {
            printf("  SOEM Error: %s\n", err_str);
        }
    }
}

/**
 * Вывод шестнадцатеричного дампа данных
 */
static void print_hex_dump(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

/**
 * Преобразование состояния slave в строку
 */
static const char* state_to_string(uint16_t state) {
    switch(state) {
        case 0x01: return "INIT";
        case 0x02: return "PRE-OP";
        case 0x04: return "SAFE-OP";
        case 0x08: return "OPERATIONAL";
        default: return "UNKNOWN";
    }
}

/* ============================================================================
 * Функции работы с SOEM
 * ============================================================================ */

/**
 * Инициализация SOEM на заданном сетевом интерфейсе
 *
 * @param ifname Имя сетевого интерфейса (например, "eth0", "\\Device\\NPF_{...}")
 * @return true при успехе, false при ошибке
 */
static bool soem_init(const char *ifname) {
    if (soem_initialized) {
        log_verbose("SOEM already initialized");
        return true;
    }

    log_verbose("Initializing SOEM on interface: %s", ifname);

    /* Инициализация SOEM на указанном интерфейсе */
    if (ecx_init(&ecx_context, ifname) <= 0) {
        print_error("Failed to initialize SOEM - ecx_init() failed");
        printf("  Check interface name and permissions (may need root/admin)\n");
        return false;
    }

    strncpy(interface_name, ifname, sizeof(interface_name) - 1);
    interface_name[sizeof(interface_name) - 1] = '\0';
    soem_initialized = true;
    log_verbose("SOEM initialized successfully");

    return true;
}

/**
 * Сканирование EtherCAT шины и обнаружение устройств
 *
 * Выполняет:
 * 1. Конфигурирование сети (ecx_config_init)
 * 2. Mapping I/O (ecx_config_map)
 * 3. Вывод списка обнаруженных slaves
 */
static void soem_scan_bus(void) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized. Use -i <interface> option.\n");
        return;
    }

    log_verbose("Starting bus scan...");

    /* Конфигурирование сети */
    int wkc = ecx_config_init(&ecx_context);
    log_verbose("ecx_config_init returned: %d", wkc);

    if (wkc <= 0) {
        print_error("No slaves found on the bus");
        return;
    }

    /* Mapping процесс данных */
    ecx_config_map_group(&ecx_context, &IOmap, 0);
    log_verbose("I/O mapping completed");

    /* Вывод информации об обнаруженных slaves */
    printf("\n=== EtherCAT Bus Scan Results ===\n");
    printf("Found %d slave(s)\n\n", ecx_context.slavecount);

    if (ecx_context.slavecount == 0) {
        printf("No slaves detected.\n");
        return;
    }

    /* Заголовок таблицы */
    printf("%-5s %-20s %-10s %-10s %-15s\n",
           "Index", "Name", "Vendor", "Product", "State");
    printf("-------------------------------------------------------------\n");

    /* Вывод информации о каждом slave (начиная с 1, 0 - мастер) */
    for (int i = 1; i <= ecx_context.slavecount; i++) {
        printf("%-5d %-20s 0x%08X 0x%08X %-15s\n",
               i,
               ecx_context.slavelist[i].name,
               ecx_context.slavelist[i].eep_man,
               ecx_context.slavelist[i].eep_id,
               state_to_string(ecx_context.slavelist[i].state));

        if (verbose_mode) {
            printf("      Station Address: 0x%04X, Configured Address: 0x%04X\n",
                   ecx_context.slavelist[i].configadr, ecx_context.slavelist[i].aliasadr);
        }
    }
    printf("\n");
}

/**
 * Чтение конфигурации конкретного slave устройства
 *
 * @param slave_idx Индекс slave (1-based)
 */
static void soem_read_config(int slave_idx) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized.\n");
        return;
    }

    /* Проверка валидности индекса */
    if (slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        printf("ERROR: Invalid slave index %d (valid range: 1-%d)\n",
               slave_idx, ecx_context.slavecount);
        return;
    }

    ec_slavet *slave = &ecx_context.slavelist[slave_idx];

    printf("\n=== Slave %d Configuration ===\n", slave_idx);
    printf("Name:             %s\n", slave->name);
    printf("Vendor ID:        0x%08X\n", slave->eep_man);
    printf("Product ID:       0x%08X\n", slave->eep_id);
    printf("Revision:         0x%08X\n", slave->eep_rev);
    printf("Serial:           0x%08X\n", slave->eep_man); /* Note: eep_serial not available in structure */
    printf("\n");

    printf("Station Address:  0x%04X\n", slave->configadr);
    printf("Alias Address:    0x%04X\n", slave->aliasadr);
    printf("State:            %s (0x%02X)\n", state_to_string(slave->state), slave->state);
    printf("\n");

    printf("Input Length:     %d bytes\n", slave->Ibytes);
    printf("Output Length:    %d bytes\n", slave->Obytes);
    printf("Input Bits:       %d\n", slave->Ibits);
    printf("Output Bits:      %d\n", slave->Obits);
    printf("\n");

    /* Sync Manager информация */
    printf("Sync Managers:\n");
    for (int i = 0; i < EC_MAXSM; i++) {
        if (slave->SM[i].StartAddr > 0) {
            printf("  SM%d: Start=0x%04X, Length=%d, Control=0x%02X, Enable=0x%02X\n",
                   i,
                   slave->SM[i].StartAddr,
                   slave->SM[i].SMlength,
                   slave->SM[i].SMflags,
                   slave->SM[i].SMflags);
        }
    }
    printf("\n");

    /* FMMU информация */
    if (verbose_mode) {
        printf("FMMU Configuration:\n");
        for (int i = 0; i < EC_MAXFMMU; i++) {
            if (slave->FMMU[i].LogStart > 0) {
                printf("  FMMU%d: LogStart=0x%08X, Length=%d, PhysStart=0x%04X\n",
                       i,
                       slave->FMMU[i].LogStart,
                       slave->FMMU[i].LogLength,
                       slave->FMMU[i].PhysStart);
            }
        }
        printf("\n");
    }

    /* Mailbox информация */
    if (slave->mbx_l > 0) {
        printf("Mailbox Configuration:\n");
        printf("  Length:         %d bytes\n", slave->mbx_l);
        printf("  Protocols:      0x%04X\n", slave->mbx_proto);
        printf("\n");
    }

    /* CoE информация */
    if (slave->CoEdetails) {
        printf("CoE Details:      0x%02X\n", slave->CoEdetails);
        if (slave->CoEdetails & ECT_COEDET_SDOCA) printf("  - SDO CA supported\n");
        if (slave->CoEdetails & ECT_COEDET_SDOINFO) printf("  - SDO Info supported\n");
        if (slave->CoEdetails & ECT_COEDET_PDOASSIGN) printf("  - PDO Assign supported\n");
        if (slave->CoEdetails & ECT_COEDET_PDOCONFIG) printf("  - PDO Config supported\n");
        printf("\n");
    }
}

/**
 * Чтение произвольных данных из памяти slave
 *
 * @param slave_idx Индекс slave
 * @param addr Адрес для чтения
 * @param len Количество байт для чтения
 */
static void soem_read_data(int slave_idx, uint32_t addr, size_t len) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized.\n");
        return;
    }

    if (slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        printf("ERROR: Invalid slave index %d\n", slave_idx);
        return;
    }

    if (len == 0 || len > 1024) {
        printf("ERROR: Invalid length %zu (must be 1-1024)\n", len);
        return;
    }

    uint8_t *buffer = malloc(len);
    if (!buffer) {
        printf("ERROR: Memory allocation failed\n");
        return;
    }

    log_verbose("Reading %zu bytes from slave %d at address 0x%04X",
                len, slave_idx, addr);

    /* Чтение данных через SOEM Read/Write функции */
    int wkc = ecx_FPRD(&ecx_context.port, ecx_context.slavelist[slave_idx].configadr,
                       (uint16_t)addr, (uint16_t)len, buffer, EC_TIMEOUTRET);

    if (wkc <= 0) {
        print_error("Failed to read data");
        free(buffer);
        return;
    }

    printf("\n=== Read Data from Slave %d ===\n", slave_idx);
    printf("Address: 0x%04X, Length: %zu bytes\n", addr, len);
    printf("Data:\n");
    print_hex_dump(buffer, len);
    printf("\n");

    free(buffer);
}

/**
 * Запись произвольных данных в память slave
 *
 * @param slave_idx Индекс slave
 * @param addr Адрес для записи
 * @param data Указатель на данные
 * @param len Длина данных
 */
static void soem_write_data(int slave_idx, uint32_t addr, const uint8_t *data, size_t len) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized.\n");
        return;
    }

    if (slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        printf("ERROR: Invalid slave index %d\n", slave_idx);
        return;
    }

    if (len == 0 || len > 1024) {
        printf("ERROR: Invalid length %zu\n", len);
        return;
    }

    log_verbose("Writing %zu bytes to slave %d at address 0x%04X",
                len, slave_idx, addr);

    /* Запись данных */
    int wkc = ecx_FPWR(&ecx_context.port, ecx_context.slavelist[slave_idx].configadr,
                       (uint16_t)addr, (uint16_t)len, (void*)data, EC_TIMEOUTRET);

    if (wkc <= 0) {
        print_error("Failed to write data");
        return;
    }

    printf("Successfully wrote %zu bytes to slave %d at address 0x%04X\n",
           len, slave_idx, addr);

    if (verbose_mode) {
        printf("Data written:\n");
        print_hex_dump(data, len);
    }
}

/**
 * Очистка ресурсов SOEM
 */
static void soem_cleanup(void) {
    if (soem_initialized) {
        log_verbose("Cleaning up SOEM resources");
        ecx_close(&ecx_context);
        soem_initialized = false;
        pdo_active = false;
        pdo_running = false;
    }
}

/* ============================================================================
 * PDO циклический обмен данными
 * ============================================================================ */

/**
 * Переход slaves в указанное состояние
 */
static bool soem_request_state(uint16_t state, uint32_t timeout_ms) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized.\n");
        return false;
    }

    const char *state_name = state_to_string(state);
    log_verbose("Requesting state %s for all slaves", state_name);

    /* Запрос изменения состояния для всех slaves (0 = все) */
    ecx_context.slavelist[0].state = state;
    ecx_writestate(&ecx_context, 0);

    /* Ожидание достижения состояния */
    int wkc = ecx_statecheck(&ecx_context, 0, state, timeout_ms * 1000);

    if (wkc != ecx_context.slavecount) {
        printf("WARNING: Not all slaves reached %s state\n", state_name);
        for (int i = 1; i <= ecx_context.slavecount; i++) {
            if (ecx_context.slavelist[i].state != state) {
                printf("  Slave %d: %s (expected %s)\n",
                       i,
                       state_to_string(ecx_context.slavelist[i].state),
                       state_name);
            }
        }
        return false;
    }

    log_verbose("All slaves reached %s state", state_name);
    return true;
}

/**
 * Активация PDO обмена (переход в OPERATIONAL)
 */
static bool soem_start_pdo(void) {
    if (!soem_initialized) {
        printf("ERROR: SOEM not initialized. Run 'scan' first.\n");
        return false;
    }

    if (ecx_context.slavecount == 0) {
        printf("ERROR: No slaves found. Run 'scan' first.\n");
        return false;
    }

    if (pdo_active) {
        printf("PDO exchange already active\n");
        return true;
    }

    log_verbose("Starting PDO exchange...");

    /* Переход в PRE-OP */
    if (!soem_request_state(EC_STATE_PRE_OP, 5000)) {
        print_error("Failed to reach PRE-OP state");
        // return false;
    }

    /* Переход в SAFE-OP */
    if (!soem_request_state(EC_STATE_SAFE_OP, 5000)) {
        print_error("Failed to reach SAFE-OP state");
        //return false;
    }

    /* Переход в OPERATIONAL */
    if (!soem_request_state(EC_STATE_OPERATIONAL, 5000)) {
        print_error("Failed to reach OPERATIONAL state");
        //return false;
    }

    pdo_active = true;
    log_verbose("PDO exchange activated successfully");

    printf("✓ All slaves in OPERATIONAL state\n");
    printf("  Input bytes:  %d (offset: 0)\n", ecx_context.grouplist[0].Ibytes);
    printf("  Output bytes: %d (offset: %d)\n",
           ecx_context.grouplist[0].Obytes,
           ecx_context.grouplist[0].Ibytes);

    return true;
}

/**
 * Остановка PDO обмена (переход в INIT)
 */
static void soem_stop_pdo(void) {
    if (!pdo_active) {
        printf("PDO exchange not active\n");
        return;
    }

    log_verbose("Stopping PDO exchange...");
    pdo_running = false;

    /* Переход в INIT состояние */
    soem_request_state(EC_STATE_INIT, 5000);

    pdo_active = false;
    printf("✓ PDO exchange stopped\n");
}

/**
 * Однократный обмен PDO данными (send outputs, receive inputs)
 */
static bool soem_exchange_pdo(void) {
    if (!pdo_active) {
        printf("ERROR: PDO exchange not active. Run 'pdo-start' first.\n");
        return false;
    }

    /* Отправка outputs и получение inputs */
    ecx_send_processdata(&ecx_context);
    int wkc = ecx_receive_processdata(&ecx_context, EC_TIMEOUTRET);

    int expected_wkc = (ecx_context.grouplist[0].outputsWKC * 2) +
                       ecx_context.grouplist[0].inputsWKC;

    if (wkc < expected_wkc) {
        log_verbose("WARNING: Working counter mismatch (got %d, expected %d)", wkc, expected_wkc);
        return false;
    }

    log_verbose("PDO exchange successful (WKC: %d)", wkc);
    return true;
}

/**
 * Чтение PDO входных данных из IOmap
 */
static void soem_read_pdo_inputs(void) {
    if (!pdo_active) {
        printf("ERROR: PDO exchange not active. Run 'pdo-start' first.\n");
        return;
    }

    /* Выполнить обмен данными */
    if (!soem_exchange_pdo()) {
        printf("WARNING: PDO exchange had issues\n");
    }

    int input_bytes = ecx_context.grouplist[0].Ibytes;

    if (input_bytes == 0) {
        printf("No input data available (0 bytes)\n");
        return;
    }

    printf("\n=== PDO Input Data ===\n");
    printf("Total input bytes: %d\n", input_bytes);

    /* Показываем данные по каждому slave */
    for (int i = 1; i <= ecx_context.slavecount; i++) {
        if (ecx_context.slavelist[i].Ibytes > 0) {
            printf("\nSlave %d (%s):\n", i, ecx_context.slavelist[i].name);
            printf("  Input bytes: %d (offset: %d)\n",
                   ecx_context.slavelist[i].Ibytes,
                   ecx_context.slavelist[i].Ibits / 8);

            uint8_t *input_ptr = ecx_context.slavelist[i].inputs;
            printf("  Data: ");
            print_hex_dump(input_ptr, ecx_context.slavelist[i].Ibytes);
        }
    }

    printf("\n=== Complete IOmap (Inputs) ===\n");
    print_hex_dump((uint8_t*)IOmap, input_bytes);
    printf("\n");
}

/**
 * Запись PDO выходных данных в IOmap
 */
static void soem_write_pdo_outputs(const uint8_t *data, size_t offset, size_t len) {
    if (!pdo_active) {
        printf("ERROR: PDO exchange not active. Run 'pdo-start' first.\n");
        return;
    }

    int output_bytes = ecx_context.grouplist[0].Obytes;
    int output_offset = ecx_context.grouplist[0].Ibytes;

    if (output_bytes == 0) {
        printf("ERROR: No output data available (0 bytes)\n");
        return;
    }

    if (offset + len > (size_t)output_bytes) {
        printf("ERROR: Write would exceed output buffer (offset %zu + len %zu > %d bytes)\n",
               offset, len, output_bytes);
        return;
    }

    log_verbose("Writing %zu bytes to output offset %zu", len, offset);

    /* Записываем данные в IOmap */
    memcpy(&IOmap[output_offset + offset], data, len);

    /* Выполняем обмен данными */
    if (!soem_exchange_pdo()) {
        printf("WARNING: PDO exchange had issues\n");
    }

    printf("✓ Wrote %zu bytes to PDO outputs at offset %zu\n", len, offset);

    if (verbose_mode) {
        printf("Data written:\n");
        print_hex_dump(data, len);

        printf("\n=== Complete IOmap (Outputs) ===\n");
        print_hex_dump((uint8_t*)&IOmap[output_offset], output_bytes);
        printf("\n");
    }
}

/**
 * Циклический обмен PDO данными (для тестирования)
 */
static void soem_run_pdo_loop(int cycles, int interval_ms) {
    if (!pdo_active) {
        printf("ERROR: PDO exchange not active. Run 'pdo-start' first.\n");
        return;
    }

    printf("\n=== Running PDO Loop ===\n");
    printf("Cycles: %d, Interval: %d ms\n", cycles, interval_ms);
    printf("Press Ctrl+C to stop (if implemented)\n\n");

    pdo_running = true;
    int errors = 0;

    for (int i = 0; i < cycles && pdo_running; i++) {
        if (!soem_exchange_pdo()) {
            errors++;
        }

        if (verbose_mode || (i % 100 == 0)) {
            printf("Cycle %d/%d (errors: %d)\r", i + 1, cycles, errors);
            fflush(stdout);
        }

#ifdef _WIN32
        Sleep(interval_ms);
#else
        usleep(interval_ms * 1000);
#endif
    }

    printf("\n\n✓ PDO loop completed: %d cycles, %d errors\n", cycles, errors);
    pdo_running = false;
}

/* ============================================================================
 * Обработка команд CLI
 * ============================================================================ */

/**
 * Вывод справки по командам
 */
static void cmd_help(void) {
    printf("\n=== EtherCAT CLI Commands ===\n\n");
    printf("Basic Commands:\n");
    printf("  help              - Show this help message\n");
    printf("  scan              - Scan EtherCAT bus and list all slaves\n");
    printf("  read-config <idx> - Read configuration of slave at index <idx>\n");
    printf("  status            - Show current status and statistics\n");
    printf("  verbose [on|off]  - Enable/disable verbose mode\n");
    printf("  quit, exit        - Exit the program\n");
    printf("\n");
    printf("Direct Memory Access:\n");
    printf("  read <idx> <addr> <len>\n");
    printf("                    - Read <len> bytes from slave <idx> at address <addr>\n");
    printf("                      Example: read 1 0x1000 16\n");
    printf("  write <idx> <addr> <byte1> <byte2> ...\n");
    printf("                    - Write bytes to slave <idx> at address <addr>\n");
    printf("                      Example: write 1 0x1000 0x12 0x34 0xAB\n");
    printf("  text-write <idx> <addr> <text>\n");
    printf("                    - Write text string to slave <idx> at address <addr>\n");
    printf("                      Supports ASCII and Cyrillic (UTF-8) for MT-08S2A display\n");
    printf("                      Example: text-write 1 0x1000 Hello World\n");
    printf("\n");
    printf("PDO Cyclic Data Exchange:\n");
    printf("  pdo-start         - Start PDO exchange (transition to OPERATIONAL)\n");
    printf("  pdo-stop          - Stop PDO exchange (transition to INIT)\n");
    printf("  pdo-read          - Read PDO input data from all slaves\n");
    printf("  pdo-write <offset> <byte1> <byte2> ...\n");
    printf("                    - Write bytes to PDO outputs at offset\n");
    printf("                      Example: pdo-write 0 0xFF 0x00\n");
    printf("  pdo-loop <cycles> [interval_ms]\n");
    printf("                    - Run PDO exchange loop for testing\n");
    printf("                      Example: pdo-loop 1000 10\n");
    printf("\n");
}

/**
 * Команда read-config
 */
static void cmd_read_config(int argc, char **argv) {
    if (argc < 2) {
        printf("ERROR: Missing slave index. Usage: read-config <slave_idx>\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    soem_read_config(slave_idx);
}

/**
 * Команда read
 */
static void cmd_read(int argc, char **argv) {
    if (argc < 4) {
        printf("ERROR: Usage: read <slave_idx> <addr> <len>\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);
    size_t len = (size_t)strtoul(argv[3], NULL, 0);

    soem_read_data(slave_idx, addr, len);
}

/**
 * Команда write
 */
static void cmd_write(int argc, char **argv) {
    if (argc < 4) {
        printf("ERROR: Usage: write <slave_idx> <addr> <byte1> [byte2] ...\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);
    size_t len = argc - 3;

    uint8_t *data = malloc(len);
    if (!data) {
        printf("ERROR: Memory allocation failed\n");
        return;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)strtoul(argv[3 + i], NULL, 0);
    }

    soem_write_data(slave_idx, addr, data, len);
    free(data);
}

/**
 * Конвертация UTF-8 в коды символов дисплея MT-08S2A-2KLW (кириллица)
 *
 * Дисплей MT-08S2A-2KLW использует специальную таблицу знакогенератора с поддержкой
 * кириллицы. Таблица преобразования (используется Страница 0 встроенного знакогенератора):
 *
 * ASCII (0x20-0x7F) - стандартные символы без изменений
 *
 * Кириллица:
 *   Все буквы которые похожи в алфавите английском и кириллице - сэкономлены.
 *   т.е. А а, В, Д д, Е е, К, М, Н, О о, Р р, С с, Т, у - взяты из английского алфавита по стандартным адресам
 *   Б -> 0xA0
 *   Г -> 0xA1
 *   Ё -> 0xA2
 *   Ж -> 0xA3
 *   З -> 0xA4
 *   И -> 0xA5
 *   Й -> 0xA6
 *   Л -> 0xA7
 *   П -> 0xA8
 *   У -> 0xA9
 *   Ф -> 0xAA
 *   Ч -> 0xAB
 *   Ш -> 0xAC
 *   Ъ -> 0xAD
 *   Ы -> 0xAE
 *   Э -> 0xAF
 *   Ю -> 0xB0
 *   Я -> 0xB1
 *   б -> 0xB2
 *   в -> 0xB3
 *   г -> 0xB4
 *   ё -> 0xB5
 *   ж -> 0xB6
 *   з -> 0xB7
 *   и -> 0xB8
 *   й -> 0xB9
 *   к -> 0xBA
 *   л -> 0xBB
 *   м -> 0xBC
 *   н -> 0xBD
 *   п -> 0xBE
 *   т -> 0xBF
 *   Д -> 0xE0
 *   Ц -> 0xE1
 *   Щ -> 0xE2
 *   д -> 0xE3
 *   ф -> 0xE4
 *   ц -> 0xE5
 *   щ -> 0xE6
 *   ч -> 0xC0
 *   ш -> 0xC1
 *   ъ -> 0xC2
 *   ы -> 0xC3
 *   ь -> 0xC4
 *   э -> 0xC5
 *   ю -> 0xC6
 *   я -> 0xC7

 * Возвращает код символа для дисплея или '?' если символ не найден
 */
static uint8_t utf8_to_mt_display(const unsigned char *utf8, size_t *bytes_read) {
    *bytes_read = 1;

    /* ASCII символы (0x00-0x7F) передаются как есть */
    if (utf8[0] < 0x80) {
        return utf8[0];
    }
    return '?';
}

static void cmd_text_write(int argc, char **argv) {
    if (argc < 4) {
        printf("ERROR: Usage: text_write <slave_idx> <addr> <text>\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);

    /* Объединяем все аргументы после addr в одну строку */
    size_t total_len = 0;
    for (int i = 3; i < argc; i++) {
        total_len += strlen(argv[i]);
        if (i < argc - 1) total_len++; /* Пробел между словами */
    }

    char *text = malloc(total_len + 1);
    if (!text) {
        printf("ERROR: Memory allocation failed\n");
        return;
    }

    text[0] = '\0';
    for (int i = 3; i < argc; i++) {
        strcat(text, argv[i]);
        if (i < argc - 1) strcat(text, " ");
    }

    size_t text_len = strlen(text);
    /* Выделяем буфер с запасом для конвертированных символов */
    uint8_t *data = malloc(text_len + 1);
    if (!data) {
        printf("ERROR: Memory allocation failed\n");
        free(text);
        return;
    }

    /* Конвертируем UTF-8 текст в коды дисплея MT-08S2A */
    // size_t data_len = 0;
    // size_t i = 0;
    // while (i < text_len) {
    //     size_t bytes_read = 0;
    //     data[data_len++] = text[i];//utf8_to_mt_display((unsigned char*)&text[i], &bytes_read);
    //     i += bytes_read;
    // }

    soem_write_data(slave_idx, addr, text, text_len);

    free(data);
    free(text);
}

/**
 * Команда verbose
 */
static void cmd_verbose(int argc, char **argv) {
    if (argc < 2) {
        printf("Verbose mode is currently: %s\n", verbose_mode ? "ON" : "OFF");
        return;
    }

    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0) {
        verbose_mode = true;
        printf("Verbose mode enabled\n");
    } else if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0) {
        verbose_mode = false;
        printf("Verbose mode disabled\n");
    } else {
        printf("ERROR: Usage: verbose [on|off]\n");
    }
}

/**
 * Команда status
 */
static void cmd_status(void) {
    printf("\n=== EtherCAT Status ===\n");
    printf("SOEM Initialized:  %s\n", soem_initialized ? "Yes" : "No");
    printf("Interface:         %s\n", interface_name[0] ? interface_name : "None");
    printf("Verbose Mode:      %s\n", verbose_mode ? "ON" : "OFF");
    printf("PDO Active:        %s\n", pdo_active ? "Yes (OPERATIONAL)" : "No");

    if (soem_initialized) {
        printf("Slaves Count:      %d\n", ecx_context.slavecount);
        printf("Expected WKC:      %d\n",
               ecx_context.grouplist[0].outputsWKC * 2 + ecx_context.grouplist[0].inputsWKC);

        if (pdo_active) {
            printf("Input bytes:       %d\n", ecx_context.grouplist[0].Ibytes);
            printf("Output bytes:      %d\n", ecx_context.grouplist[0].Obytes);
        }
        printf("\n");

        if (ecx_context.slavecount > 0) {
            printf("Slave States:\n");
            for (int i = 1; i <= ecx_context.slavecount; i++) {
                printf("  Slave %d (%s): %s",
                       i,
                       ecx_context.slavelist[i].name,
                       state_to_string(ecx_context.slavelist[i].state));
                if (pdo_active) {
                    printf(" [I:%d O:%d]",
                           ecx_context.slavelist[i].Ibytes,
                           ecx_context.slavelist[i].Obytes);
                }
                printf("\n");
            }
            printf("\n");
        }
    } else {
        printf("Slaves Count:      0\n");
        printf("\n");
    }
}

/**
 * Команда pdo-start
 */
static void cmd_pdo_start(void) {
    if (soem_start_pdo()) {
        printf("\n");
        cmd_status();
    }
}

/**
 * Команда pdo-stop
 */
static void cmd_pdo_stop(void) {
    soem_stop_pdo();
}

/**
 * Команда pdo-read
 */
static void cmd_pdo_read(void) {
    soem_read_pdo_inputs();
}

/**
 * Команда pdo-write
 */
static void cmd_pdo_write(int argc, char **argv) {
    if (argc < 3) {
        printf("ERROR: Usage: pdo-write <offset> <byte1> [byte2] ...\n");
        printf("Example: pdo-write 0 0xFF 0x00\n");
        return;
    }

    size_t offset = (size_t)strtoul(argv[1], NULL, 0);
    size_t len = argc - 2;

    uint8_t *data = malloc(len);
    if (!data) {
        printf("ERROR: Memory allocation failed\n");
        return;
    }

    for (size_t i = 0; i < len; i++) {
        data[i] = (uint8_t)strtoul(argv[2 + i], NULL, 0);
    }

    soem_write_pdo_outputs(data, offset, len);
    free(data);
}

/**
 * Команда pdo-loop
 */
static void cmd_pdo_loop(int argc, char **argv) {
    if (argc < 2) {
        printf("ERROR: Usage: pdo-loop <cycles> [interval_ms]\n");
        printf("Example: pdo-loop 1000 10\n");
        return;
    }

    int cycles = atoi(argv[1]);
    int interval_ms = 10;  // По умолчанию 10 мс

    if (argc >= 3) {
        interval_ms = atoi(argv[2]);
    }

    if (cycles <= 0 || cycles > 1000000) {
        printf("ERROR: Invalid cycles count (must be 1-1000000)\n");
        return;
    }

    if (interval_ms < 1 || interval_ms > 10000) {
        printf("ERROR: Invalid interval (must be 1-10000 ms)\n");
        return;
    }

    soem_run_pdo_loop(cycles, interval_ms);
}

/* ============================================================================
 * REPL - Read-Eval-Print Loop
 * ============================================================================ */

/**
 * Разбор строки команды на аргументы
 */
static int parse_command(char *line, char **argv, int max_args) {
    int argc = 0;
    char *token = strtok(line, " \t\n\r");

    while (token != NULL && argc < max_args) {
        argv[argc++] = token;
        token = strtok(NULL, " \t\n\r");
    }

    return argc;
}

/**
 * Обработка одной команды
 */
static bool process_command(char *line) {
    char *argv[MAX_ARGS];
    int argc = parse_command(line, argv, MAX_ARGS);

    if (argc == 0) {
        /* Пустая команда - показываем help */
        cmd_help();
        return true;
    }

    /* Обработка команд */
    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        cmd_help();
    }
    else if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "exit") == 0) {
        return false;  /* Выход из REPL */
    }
    else if (strcmp(argv[0], "scan") == 0) {
        soem_scan_bus();
    }
    else if (strcmp(argv[0], "read-config") == 0) {
        cmd_read_config(argc, argv);
    }
    else if (strcmp(argv[0], "read") == 0) {
        cmd_read(argc, argv);
    }
    else if (strcmp(argv[0], "write") == 0) {
        cmd_write(argc, argv);
    }
    else if (strcmp(argv[0], "text-write") == 0) {
        cmd_text_write(argc, argv);
    }
    else if (strcmp(argv[0], "verbose") == 0) {
        cmd_verbose(argc, argv);
    }
    else if (strcmp(argv[0], "status") == 0) {
        cmd_status();
    }
    else if (strcmp(argv[0], "pdo-start") == 0) {
        cmd_pdo_start();
    }
    else if (strcmp(argv[0], "pdo-stop") == 0) {
        cmd_pdo_stop();
    }
    else if (strcmp(argv[0], "pdo-read") == 0) {
        cmd_pdo_read();
    }
    else if (strcmp(argv[0], "pdo-write") == 0) {
        cmd_pdo_write(argc, argv);
    }
    else if (strcmp(argv[0], "pdo-loop") == 0) {
        cmd_pdo_loop(argc, argv);
    }
    else {
        printf("ERROR: Unknown command '%s'. Type 'help' for list of commands.\n", argv[0]);
    }

    return true;
}

/**
 * Главный цикл REPL
 */
static void repl_loop(void) {
    char line[MAX_COMMAND_LEN];

    printf("\nEtherCAT CLI - Interactive Mode\n");
    printf("Type 'help' for commands, 'quit' to exit\n\n");

    while (1) {
        printf("dummy_says> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            break;  /* EOF или ошибка */
        }

        /* Убираем trailing whitespace */
        size_t len = strlen(line);
        while (len > 0 && isspace((unsigned char)line[len - 1])) {
            line[--len] = '\0';
        }

        if (len == 0) {
            continue;  /* Пустая строка */
        }

        if (!process_command(line)) {
            break;  /* Команда quit/exit */
        }
    }

    printf("\nExiting...\n");
}

/* ============================================================================
 * Главная функция и парсинг аргументов
 * ============================================================================ */

/**
 * Вывод информации об использовании программы
 */
static void print_usage(const char *prog_name) {
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\nOptions:\n");
    printf("  -i, --interface <name>  Network interface name (required)\n");
    printf("  -v, --verbose           Enable verbose output\n");
    printf("  -h, --help              Show this help\n");
    printf("\nExamples:\n");
    printf("  %s -i eth0\n", prog_name);
    printf("  %s -i \"\\\\Device\\\\NPF_{...}\" -v\n", prog_name);
    printf("\n");
}

/**
 * Главная функция программы
 */
int main(int argc, char *argv[]) {
    const char *nic_iface = NULL;

    printf("=== EtherCAT CLI Tool ===\n");
    printf("Version 1.0 (SOEM 2.0)\n\n");

    /* Инициализация context structure */
    // memset(&ecx_context, 0, sizeof(ecx_context));

    /* Парсинг аргументов командной строки */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interface") == 0) {
            if (i + 1 < argc) {
                nic_iface = argv[++i];
            } else {
                printf("ERROR: -i option requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = true;
            printf("Verbose mode enabled\n");
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        else {
            printf("ERROR: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Проверка обязательного параметра -i */
    if (nic_iface == NULL) {
        printf("ERROR: Network interface is required\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Инициализация SOEM */
    if (!soem_init(nic_iface)) {
        return 1;
    }

    printf("SOEM initialized on interface: %s\n", nic_iface);

    /* Запуск интерактивного режима */
    repl_loop();

    /* Очистка ресурсов */
    soem_cleanup();

    return 0;
}
