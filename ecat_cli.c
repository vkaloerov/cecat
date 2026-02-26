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
#include <termios.h>
#include <pwd.h>
#endif

#include "soem/soem.h"
#include "my_hex_dump.h"

/* ============================================================================
 * Глобальные переменные для состояния SOEM
 * ============================================================================ */

#define MAX_IO_MAP_SIZE 4096
#define MAX_COMMAND_LEN 256
#define MAX_ARGS 32
#define MAX_HISTORY 100
#define HISTORY_FILENAME ".ecat_cli_history"

static char IOmap[MAX_IO_MAP_SIZE];  /* Буфер для I/O mapping */
static bool soem_initialized = false; /* Флаг инициализации SOEM */
static bool verbose_mode = false;     /* Флаг verbose режима */
static char interface_name[128] = "";  /* Имя сетевого интерфейса */
static bool pdo_active = false;       /* Флаг активности PDO обмена */
static volatile bool pdo_running = false; /* Флаг работы PDO цикла */
static unsigned int expectedWKC = 0;

/* SOEM 2.0 context structure */
static ecx_contextt ecx_context;


/*
 * Структура для взаимодействия с PDO Leadshine EM3E-556
 */
OSAL_PACKED_BEGIN
typedef struct OSAL_PACKED
{
    struct
    {
        uint16_t ControlWord;
        uint32_t ProfileTargetPosition;
        uint16_t TouchProbeFunction;
    } outputs;
    struct
    {
        uint16_t LastErrorCode;
        uint16_t StatusWord;
        int8_t ModesOfOperationDisplay;
        int32_t ActualPosition;
        uint16_t TouchProbeStatus;
        int32_t TouchProbe1PositiveValue;
        uint32_t DigitalInputStatus;

    } inputs;
} process_data_t;
OSAL_PACKED_END

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
 * История команд (Command History) - POSIX реализация с сохранением в файл
 * ============================================================================ */

typedef struct {
    char commands[MAX_HISTORY][MAX_COMMAND_LEN];
    int count;           /* Количество команд в истории */
    int current;         /* Текущий индекс при навигации */
} CommandHistory;

static CommandHistory cmd_history = { .count = 0, .current = -1 };
static char history_filepath[512] = "";

/**
 * Получить путь к файлу истории в домашней директории
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
 * Загрузить историю команд из файла
 */
static void history_load(void) {
    if (strlen(history_filepath) == 0) {
        history_get_filepath();
    }

    FILE *f = fopen(history_filepath, "r");
    if (!f) return;  /* Файл еще не существует - это OK */

    cmd_history.count = 0;
    while (cmd_history.count < MAX_HISTORY) {
        if (fgets(cmd_history.commands[cmd_history.count],
                  MAX_COMMAND_LEN, f) == NULL) {
            break;
        }

        /* Убираем newline */
        size_t len = strlen(cmd_history.commands[cmd_history.count]);
        if (len > 0 && cmd_history.commands[cmd_history.count][len-1] == '\n') {
            cmd_history.commands[cmd_history.count][len-1] = '\0';
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
 * Сохранить историю команд в файл
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
 * Добавить команду в историю и сохранить
 */
static void history_add(const char *cmd) {
    /* Не добавляем пустые команды */
    if (!cmd || strlen(cmd) == 0) return;

    /* Не добавляем дубликаты подряд (последняя команда) */
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
        /* Циклический буфер - переписываем старейшую команду */
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
 * Получить команду из истории по смещению от конца
 * offset=1 -> последняя команда, offset=2 -> предпоследняя и т.д.
 */
static const char* history_get_back(int offset) {
    if (cmd_history.count == 0 || offset <= 0 || offset > cmd_history.count) {
        return NULL;
    }
    return cmd_history.commands[cmd_history.count - offset];
}

/**
 * Показать историю команд (для команды 'history')
 */
static void history_show(void) {
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
 * Очистить историю (удалить файл)
 */
static void history_clear(void) {
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
 * Чтение строки с поддержкой истории (UP/DOWN стрелки)
 * Только POSIX реализация (Linux, macOS, Raspberry Pi OS и т.д.)
 */
static bool read_line_with_history(char *buffer, size_t max_len) {
    struct termios orig_termios, raw_termios;

    /* Сохранить оригинальные настройки терминала */
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        /* Fallback на обычный fgets если что-то пошло не так */
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
    raw_termios.c_cc[VMIN] = 1;
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
            /* Enter - завершить ввод */
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
            /* ESC - возможно начало escape sequence */
            unsigned char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (seq[0] != '[') continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;

            if (seq[1] == 'A') {
                /* UP arrow - показать предыдущую команду */
                history_offset++;
                const char *hist = history_get_back(history_offset);
                if (hist == NULL) {
                    history_offset--;
                    continue;
                }

                /* Переместить курсор в начало строки и очистить */
                printf("\r");
                //printf("%s", prompt);
                /* Очистить остатки старого текста */
                for (int i = 0; i < pos; i++) printf(" ");
                fflush(stdout);
                /* Вернуть курсор и вывести новую команду */
                printf("\r%s%s", prompt, hist);
                strcpy(buffer, hist);
                pos = strlen(prompt) + strlen(hist);
                fflush(stdout);
            }
            else if (seq[1] == 'B') {
                /* DOWN arrow - показать следующую команду или текущую */
                if (history_offset > 0) {
                    history_offset--;
                    const char *hist;
                    if (history_offset == 0) {
                        hist = temp_buffer;
                    } else {
                        hist = history_get_back(history_offset);
                    }

                    if (hist == NULL) hist = "";

                    /* Переместить курсор в начало строки и очистить */
                    printf("\r");
                    //printf("%s", prompt);
                    /* Очистить остатки старого текста */
                    for (int i = 0; i < pos; i++) printf(" ");
                    /* Вернуть курсор и вывести новую команду */
                    printf("\r%s%s", prompt, hist);
                    strcpy(buffer, hist);
                    pos = strlen(prompt) + strlen(hist);
                    fflush(stdout);
                }
            }
        }
        else if (c >= 32 && c < 127) {
            /* Обычный символ */
            if (pos < (int)max_len - 1) {
                buffer[pos] = c;
                pos++;
                buffer[pos] = '\0';
                printf("%c", c);
                fflush(stdout);

                /* Сохранить текущий буфер при редактировании */
                strcpy(temp_buffer, buffer);
                history_offset = 0;
            }
        }
    }

    /* Восстановить настройки терминала */
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    return false;
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

    strcpy(interface_name, ifname);
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
    unsigned short sz = ecx_config_map_group(&ecx_context, &IOmap, 0);
    if (sz > MAX_IO_MAP_SIZE) {
        print_error("I/O mapping failed");
        return;
    }

    log_verbose("I/O mapping completed");

    /* Вывод информации об обнаруженных slaves */
    printf("\n=== EtherCAT Bus Scan Results ===\n");
    printf("Found %d slave(s)\n\n", ecx_context.slavecount);

    if (ecx_context.slavecount == 0) {
        printf("No slaves detected.\n");
        return;
    }
    expectedWKC = ecx_context.grouplist[0].outputsWKC * 2 + ecx_context.grouplist[0].inputsWKC;

    // как указано в документации пора попробовать установить DC
    ecx_configdc(&ecx_context);

    // проверяем что все наши инициализации привели к тому что статус у слейвов теперь SAFE_OP
    uint16_t currentState = ecx_statecheck(&ecx_context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
    if (currentState != EC_STATE_SAFE_OP) {
        printf("Failed to set init bus. Current fieldbus slaves BITWISE OR state: %s\n", state_to_string(currentState));
        return;
    }
    log_verbose("Init bus completed successfully");

    /* Перед тем как перейти в OPERATIONAL стейт необходимо проверить что слейвы имеют валидные
       process data для этого отправляем и получаем их */
    ecx_send_processdata(&ecx_context);
    ecx_receive_processdata(&ecx_context, EC_TIMEOUTRET);
    /* Finally, the operational state can be requested through ecx_writestate() using slave 0,
       which will broadcast the request to all slaves on the network */
    ecx_context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&ecx_context, 0);

    uint16_t chk = 200;
    /* wait for all slaves to reach OP state */
    do
    {
       ecx_send_processdata(&ecx_context);
       ecx_receive_processdata(&ecx_context, EC_TIMEOUTRET);
       ecx_statecheck(&ecx_context, 0, EC_STATE_OPERATIONAL, 50000);
    } while (chk-- && (ecx_context.slavelist[0].state != EC_STATE_OPERATIONAL));
    if (ecx_context.slavelist[0].state != EC_STATE_OPERATIONAL)
    {
       /* ERROR */
       printf("Failed to move slaves to operational state\n");
       printf("Current fieldbus slaves BITWISE OR state: %s\n", state_to_string(ecx_context.slavelist[0].state));
    }

     log_verbose("Slave initialization completed successfully, OPERATIONAL state reached");

     process_data_t *pd = (process_data_t *)IOmap;
     /* print inputs of pd
      * LastErrorCode;
      * StatusWord;
      * ModesOfOperationDisplay;
      * ActualPosition;
      * TouchProbeStatus;
      * TouchProbe1PositiveValue;
      * DigitalInputStatus;
      */
     printf("Inputs of pd of EM3E-556: \n");
     printf("LastErrorCode: %d\n", pd->inputs.LastErrorCode);
     printf("StatusWord: %d\n", pd->inputs.StatusWord);
     printf("ModesOfOperationDisplay: %d\n", pd->inputs.ModesOfOperationDisplay);
     printf("ActualPosition: %d\n", pd->inputs.ActualPosition);
     printf("TouchProbeStatus: %d\n", pd->inputs.TouchProbeStatus);
     printf("TouchProbe1PositiveValue: %d\n", pd->inputs.TouchProbe1PositiveValue);
     printf("DigitalInputStatus: %u\n", pd->inputs.DigitalInputStatus);


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
    for (int i = 1; i <= ecx_context.slavecount; i++) {
        ecx_context.slavelist[i].state = state;
        ecx_writestate(&ecx_context, i);
    }


    bool all_slaves_reached_state = true;
    for (int i = 1; i <= ecx_context.slavecount; i++) {
        uint16_t slave_state = ecx_statecheck(&ecx_context, i, state, timeout_ms * 1000);
        if (state != slave_state){
            printf("WARNING: Slave %d: %s (expected %s)\n", i, state_to_string(slave_state), state_name);
            all_slaves_reached_state = false;
        }
    }
    if (!all_slaves_reached_state)
        log_verbose("NOT ALL SLAVES REACHED %s STATE SEE OUTPUT ABOVE", state_name);


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
    printf("Leadshine EM3E-556 Motor Control:\n");
    printf("  motor-enable <idx>       - Enable motor drive at slave <idx>\n");
    printf("  motor-disable <idx>      - Disable motor drive\n");
    printf("  motor-run <idx> <rpm> <sec>\n");
    printf("                           - Run motor for <sec> seconds at <rpm> RPM\n");
    printf("                             Example: motor-run 1 100 10\n");
    printf("  motor-velocity <idx> <rpm>\n");
    printf("                           - Set motor velocity (+ forward, - reverse)\n");
    printf("                             Example: motor-velocity 1 200\n");
    printf("  motor-stop <idx>         - Emergency stop motor\n");
    printf("  motor-status <idx>       - Show motor status\n");
    printf("\n");
    printf("Direct SOEM Function Calls:\n");
    printf("  c-func <function> [args] - Call a SOEM library function directly\n");
    printf("      c-func soem_init <ifname>\n");
    printf("      c-func soem_cleanup\n");
    printf("      c-func soem_scan_bus\n");
    printf("      c-func soem_request_state <state> [timeout_ms]\n");
    printf("          state: 0x01=INIT 0x02=PRE-OP 0x04=SAFE-OP 0x08=OPERATIONAL\n");
    printf("          Example: c-func soem_request_state 0x02 5000\n");
    printf("      c-func soem_start_pdo\n");
    printf("      c-func soem_stop_pdo\n");
    printf("      c-func soem_exchange_pdo\n");
    printf("      c-func ecx_config_init\n");
    printf("          Initialize SOEM configuration (call after soem_scan_bus).\n");
    printf("          Shows detailed slave configuration and state information.\n");
    printf("      c-func ecx_config_map_group\n");
    printf("          Map PDO configuration and display detailed PDO mapping.\n");
    printf("          Shows input/output byte counts, WKC values, and per-slave info.\n");
    printf("\n");
    printf("Command History (POSIX systems only):\n");
    printf("  history              - Show all commands in history\n");
    printf("  history clear        - Clear command history file\n");
    printf("  UP/DOWN arrows       - Navigate through previous commands\n");
    printf("                         (History is saved to ~/.ecat_cli_history)\n");
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
#if 0
static uint8_t utf8_to_mt_display(const unsigned char *utf8, size_t *bytes_read) {
    *bytes_read = 1;

    /* ASCII символы (0x00-0x7F) передаются как есть */
    if (utf8[0] < 0x80) {
        return utf8[0];
    }
    return '?';
}
#endif


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

    soem_write_data(slave_idx, addr, (uint8_t*)text, text_len);

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
 * Leadshine EM3E-556 Stepper Motor Control Functions
 * ============================================================================ */

/* CiA 402 Control Word (0x6040) bits */
#define CW_SWITCH_ON            (1 << 0)
#define CW_ENABLE_VOLTAGE       (1 << 1)
#define CW_QUICK_STOP           (1 << 2)
#define CW_ENABLE_OPERATION     (1 << 3)
#define CW_FAULT_RESET          (1 << 7)
#define CW_HALT                 (1 << 8)

/* Status Word (0x6041) bits */
#define SW_READY_TO_SWITCH_ON   (1 << 0)
#define SW_SWITCHED_ON          (1 << 1)
#define SW_OPERATION_ENABLED    (1 << 2)
#define SW_FAULT                (1 << 3)
#define SW_VOLTAGE_ENABLED      (1 << 4)
#define SW_QUICK_STOP           (1 << 5)
#define SW_SWITCH_ON_DISABLED   (1 << 6)
#define SW_WARNING              (1 << 7)
#define SW_TARGET_REACHED       (1 << 10)

/* Operation Modes (0x6060) */
#define MODE_PROFILE_POSITION   1
#define MODE_PROFILE_VELOCITY   3
#define MODE_HOMING             6
#define MODE_CYCLIC_SYNC_POS    8

/* State machine states */
#define STATE_NOT_READY         0
#define STATE_SWITCH_ON_DISABLED 1
#define STATE_READY_TO_SWITCH_ON 2
#define STATE_SWITCHED_ON       3
#define STATE_OPERATION_ENABLED 4
#define STATE_FAULT             5
#define STATE_QUICK_STOP_ACTIVE 6

/* EM3E-556 PDO Mapping Structures */
typedef struct __attribute__((__packed__)) {
    uint16_t control_word;      /* 0x6040 Control Word */
    int32_t target_position;    /* 0x607A Target Position */
    int32_t target_velocity;    /* 0x60FF Target Velocity */
} motor_em3e_556_outputs_t;

typedef struct __attribute__((__packed__)) {
    uint16_t status_word;       /* 0x6041 Status Word */
    int32_t actual_position;    /* 0x6064 Position Actual Value */
    int32_t actual_velocity;    /* 0x606C Velocity Actual Value */
} motor_em3e_556_inputs_t;

/**
 * Get current drive state from status word
 */
static int motor_em3e_556_get_state(uint16_t status_word) {
    uint16_t state_mask = status_word & 0x6F;

    if ((state_mask & 0x4F) == 0x00) {
        return STATE_NOT_READY;
    } else if ((state_mask & 0x4F) == 0x40) {
        return STATE_SWITCH_ON_DISABLED;
    } else if ((state_mask & 0x6F) == 0x21) {
        return STATE_READY_TO_SWITCH_ON;
    } else if ((state_mask & 0x6F) == 0x23) {
        return STATE_SWITCHED_ON;
    } else if ((state_mask & 0x6F) == 0x27) {
        return STATE_OPERATION_ENABLED;
    } else if ((status_word & 0x08) != 0) {
        return STATE_FAULT;
    } else if ((status_word & SW_QUICK_STOP) == 0) {
        return STATE_QUICK_STOP_ACTIVE;
    }

    return STATE_NOT_READY;
}

/**
 * Get state name
 */
const char* motor_em3e_556_state_name(int state) {
    switch (state) {
        case STATE_NOT_READY: return "Not Ready";
        case STATE_SWITCH_ON_DISABLED: return "Switch on Disabled";
        case STATE_READY_TO_SWITCH_ON: return "Ready to Switch On";
        case STATE_SWITCHED_ON: return "Switched On";
        case STATE_OPERATION_ENABLED: return "Operation Enabled";
        case STATE_FAULT: return "Fault";
        case STATE_QUICK_STOP_ACTIVE: return "Quick Stop Active";
        default: return "Unknown";
    }
}

/**
 * Enable the drive (State machine: Switch On Disabled -> Operation Enabled)
 */
static bool motor_em3e_556_enable(int slave_idx) {
    if (!pdo_active || slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        printf("ERROR: PDO not active or invalid slave index\n");
        return false;
    }

    motor_em3e_556_outputs_t *outputs = (motor_em3e_556_outputs_t*)ecx_context.slavelist[slave_idx].outputs;
    motor_em3e_556_inputs_t *inputs = (motor_em3e_556_inputs_t*)ecx_context.slavelist[slave_idx].inputs;

    printf("Enabling drive (slave %d)...\n", slave_idx);

    /* Transition sequence */
    int max_attempts = 50;

    for (int i = 0; i < max_attempts; i++) {
        soem_exchange_pdo();

        int state = motor_em3e_556_get_state(inputs->status_word);
        printf("  State: %s (0x%04X)\r", motor_em3e_556_state_name(state), inputs->status_word);

        if (state == STATE_OPERATION_ENABLED) {
            printf("\n✓ Drive enabled successfully\n");
            return true;

        } else if (state == STATE_NOT_READY) {
            outputs->control_word = 0;
            printf("\n✗ Drive not ready. Waiting...\n");
            usleep(100000);

        } else if (state == STATE_FAULT) {
            printf("\n✗ Drive in FAULT state. Resetting...\n");
            outputs->control_word = CW_FAULT_RESET;
            soem_exchange_pdo();
            int reset_attempts = 0;
            bool fault_cleared = false;
            while (reset_attempts < 10) {
                soem_exchange_pdo();
                state = motor_em3e_556_get_state(inputs->status_word);
                if (state != STATE_FAULT) {
                    printf("  Fault cleared.\n");
                    fault_cleared = true;
                    break;
                }
                reset_attempts++;
#ifdef _WIN32
                Sleep(100);
#else
                usleep(100000);
#endif
            }
            if (!fault_cleared) {
                printf("  Failed to clear fault.\n");
            }
        } else if (state == STATE_QUICK_STOP_ACTIVE) {
            outputs->control_word = 0; // 0000h to transition to Switch on disabled
        } else if (state == STATE_SWITCH_ON_DISABLED) {
            outputs->control_word = CW_ENABLE_VOLTAGE | CW_QUICK_STOP; // 0006h
        } else if (state == STATE_READY_TO_SWITCH_ON) {
            outputs->control_word = CW_SWITCH_ON | CW_ENABLE_VOLTAGE | CW_QUICK_STOP; // 0007h
        } else if (state == STATE_SWITCHED_ON) {
            outputs->control_word = CW_SWITCH_ON | CW_ENABLE_VOLTAGE | CW_QUICK_STOP | CW_ENABLE_OPERATION; // 000Fh
        } // STATE_NOT_READY: do nothing, wait for auto-transition

#ifdef _WIN32
        Sleep(50);
#else
        usleep(50000);
#endif
    }

    printf("\n✗ Failed to enable drive (timeout)\n");
    return false;
}

/**
 * Disable the drive
 */
static bool motor_em3e_556_disable(int slave_idx) {
    if (!pdo_active || slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        return false;
    }

    motor_em3e_556_outputs_t *outputs = (motor_em3e_556_outputs_t*)ecx_context.slavelist[slave_idx].outputs;

    outputs->control_word = 0;
    outputs->target_velocity = 0;
    soem_exchange_pdo();

    printf("Drive disabled\n");
    return true;
}

/**
 * Set operation mode
 */
static bool motor_em3e_556_set_mode(int slave_idx, int8_t mode) {
    if (slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        return false;
    }

    /* Write Mode of Operation (0x6060) */
    int wkc = ecx_SDOwrite(&ecx_context, slave_idx, 0x6060, 0, FALSE, sizeof(mode), &mode, EC_TIMEOUTRXM);

    if (wkc <= 0) {
        printf("ERROR: Failed to set operation mode\n");
        return false;
    }

    const char *mode_name = "Unknown";
    switch (mode) {
        case MODE_PROFILE_POSITION: mode_name = "Profile Position"; break;
        case MODE_PROFILE_VELOCITY: mode_name = "Profile Velocity"; break;
        case MODE_HOMING: mode_name = "Homing"; break;
        case MODE_CYCLIC_SYNC_POS: mode_name = "Cyclic Sync Position"; break;
    }

    printf("Operation mode set to: %s (%d)\n", mode_name, mode);
    return true;
}

/**
 * Set target velocity (Profile Velocity mode)
 */
static bool motor_em3e_556_set_velocity(int slave_idx, int32_t velocity_rpm) {
    if (!pdo_active || slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        return false;
    }

    motor_em3e_556_outputs_t *outputs = (motor_em3e_556_outputs_t*)ecx_context.slavelist[slave_idx].outputs;

    outputs->target_velocity = velocity_rpm;

    printf("Target velocity set to: %d RPM\n", velocity_rpm);
    return true;
}

/**
 * Read current status
 */
static void motor_em3e_556_print_status(int slave_idx) {
    if (!pdo_active || slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        printf("ERROR: PDO not active or invalid slave index\n");
        return;
    }

    soem_exchange_pdo();

    motor_em3e_556_inputs_t *inputs = (motor_em3e_556_inputs_t*)ecx_context.slavelist[slave_idx].inputs;

    int state = motor_em3e_556_get_state(inputs->status_word);

    printf("\n=== EM3E-556 Status (Slave %d) ===\n", slave_idx);
    printf("State:            %s\n", motor_em3e_556_state_name(state));
    printf("Status Word:      0x%04X\n", inputs->status_word);
    printf("Actual Position:  %d counts\n", inputs->actual_position);
    printf("Actual Velocity:  %d RPM\n", inputs->actual_velocity);
    printf("\nStatus Flags:\n");
    printf("  Ready to Switch On: %s\n", (inputs->status_word & SW_READY_TO_SWITCH_ON) ? "YES" : "NO");
    printf("  Switched On:        %s\n", (inputs->status_word & SW_SWITCHED_ON) ? "YES" : "NO");
    printf("  Operation Enabled:  %s\n", (inputs->status_word & SW_OPERATION_ENABLED) ? "YES" : "NO");
    printf("  Fault:              %s\n", (inputs->status_word & SW_FAULT) ? "YES" : "NO");
    printf("  Warning:            %s\n", (inputs->status_word & SW_WARNING) ? "YES" : "NO");
    printf("  Target Reached:     %s\n", (inputs->status_word & SW_TARGET_REACHED) ? "YES" : "NO");
    printf("\n");
}

/**
 * Run motor for specified duration
 */
static bool motor_em3e_556_run_timed(int slave_idx, int32_t velocity_rpm, int duration_sec) {
    printf("\n=== Running motor for %d seconds at %d RPM ===\n", duration_sec, velocity_rpm);

    if (!motor_em3e_556_enable(slave_idx)) {
        return false;
    }

    motor_em3e_556_set_velocity(slave_idx, velocity_rpm);

    printf("Motor running");
    for (int i = 0; i < duration_sec; i++) {
        printf(".");
        fflush(stdout);

        for (int j = 0; j < 10; j++) {
            soem_exchange_pdo();
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
        }
    }
    printf(" Done!\n");

    /* Stop motor */
    motor_em3e_556_set_velocity(slave_idx, 0);
    soem_exchange_pdo();
#ifdef _WIN32
    Sleep(500);
#else
    usleep(500000);
#endif

    motor_em3e_556_print_status(slave_idx);

    return true;
}

/**
 * Emergency stop
 */
static bool motor_em3e_556_stop(int slave_idx) {
    if (!pdo_active || slave_idx < 1 || slave_idx > ecx_context.slavecount) {
        return false;
    }

    motor_em3e_556_outputs_t *outputs = (motor_em3e_556_outputs_t*)ecx_context.slavelist[slave_idx].outputs;

    /* Stop: set velocity to 0 */
    outputs->target_velocity = 0;

    /* Quick stop */
    outputs->control_word &= ~CW_QUICK_STOP;

    soem_exchange_pdo();

    printf("EMERGENCY STOP activated\n");
    return true;
}

/* CLI Command Implementations for EM3E-556 */

static void cmd_motor_enable(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: motor-enable <slave_idx>\n");
        printf("Example: motor-enable 1\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    motor_em3e_556_set_mode(slave_idx, MODE_PROFILE_VELOCITY);
    motor_em3e_556_enable(slave_idx);
}

static void cmd_motor_disable(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: motor-disable <slave_idx>\n");
        printf("Example: motor-disable 1\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    motor_em3e_556_disable(slave_idx);
}

static void cmd_motor_run(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: motor-run <slave_idx> <velocity_rpm> <duration_sec>\n");
        printf("Example: motor-run 1 100 10   (run at 100 RPM for 10 seconds)\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    int32_t velocity = atoi(argv[2]);
    int duration = atoi(argv[3]);

    motor_em3e_556_run_timed(slave_idx, velocity, duration);
}

static void cmd_motor_velocity(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: motor-velocity <slave_idx> <velocity_rpm>\n");
        printf("Example: motor-velocity 1 200\n");
        printf("  Positive = forward, Negative = reverse\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    int32_t velocity = atoi(argv[2]);

    motor_em3e_556_set_velocity(slave_idx, velocity);
}

static void cmd_motor_stop(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: motor-stop <slave_idx>\n");
        printf("Example: motor-stop 1\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    motor_em3e_556_stop(slave_idx);
}

static void cmd_motor_status(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: motor-status <slave_idx>\n");
        printf("Example: motor-status 1\n");
        return;
    }

    int slave_idx = atoi(argv[1]);
    motor_em3e_556_print_status(slave_idx);
}

/* ============================================================================
 * cmd_c_funcs — прямой вызов функций SOEM из интерактивного интерфейса
 *
 * Синтаксис:
 *   c-func <имя_функции> [аргумент1] [аргумент2] ...
 *
 * Поддерживаемые функции:
 *   soem_init <ifname>               - Инициализировать SOEM на интерфейсе
 *   soem_cleanup                     - Освободить ресурсы SOEM
 *   soem_scan_bus                    - Сканировать EtherCAT шину
 *   soem_request_state <state> [timeout_ms]
 *                                    - Перевести все slaves в указанное состояние
 *                                      state: 0x01=INIT, 0x02=PRE-OP,
 *                                             0x04=SAFE-OP, 0x08=OPERATIONAL
 *                                      timeout_ms: таймаут в мс (по умолч. 5000)
 *                                      Пример: c-func soem_request_state 0x02 5000
 *   soem_start_pdo                   - Запустить PDO обмен (→ OPERATIONAL)
 *   soem_stop_pdo                    - Остановить PDO обмен (→ INIT)
 *   soem_exchange_pdo                - Одиночный PDO обмен (send+receive)
 * ============================================================================ */

static void cmd_c_funcs(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: c-func <function_name> [args...]\n");
        printf("\n");
        printf("Available SOEM functions:\n");
        printf("  soem_init <ifname>\n");
        printf("      Initialize SOEM on a network interface.\n");
        printf("      Example: c-func soem_init eth0\n");
        printf("\n");
        printf("  soem_cleanup\n");
        printf("      Release all SOEM resources.\n");
        printf("      Example: c-func soem_cleanup\n");
        printf("\n");
        printf("  soem_scan_bus\n");
        printf("      Scan the EtherCAT bus and list discovered slaves.\n");
        printf("      Example: c-func soem_scan_bus\n");
        printf("\n");
        printf("  soem_request_state <state> [timeout_ms]\n");
        printf("      Request all slaves to transition to <state>.\n");
        printf("      state values (hex or decimal):\n");
        printf("        0x01  INIT\n");
        printf("        0x02  PRE-OP\n");
        printf("        0x04  SAFE-OP\n");
        printf("        0x08  OPERATIONAL\n");
        printf("      timeout_ms defaults to 5000 if omitted.\n");
        printf("      Example: c-func soem_request_state 0x02 5000\n");
        printf("      Example: c-func soem_request_state 0x08\n");
        printf("\n");
        printf("  soem_start_pdo\n");
        printf("      Start PDO exchange (transitions slaves to OPERATIONAL).\n");
        printf("      Example: c-func soem_start_pdo\n");
        printf("\n");
        printf("  soem_stop_pdo\n");
        printf("      Stop PDO exchange (transitions slaves to INIT).\n");
        printf("      Example: c-func soem_stop_pdo\n");
        printf("\n");
        printf("  soem_exchange_pdo\n");
        printf("      Perform a single PDO send/receive cycle.\n");
        printf("      Example: c-func soem_exchange_pdo\n");
        printf("\n");
        printf("  ecx_config_init\n");
        printf("      Initialize SOEM configuration (must call after soem_scan_bus).\n");
        printf("      Example: c-func ecx_config_init\n");
        printf("\n");
        printf("  ecx_config_map_group\n");
        printf("      Map PDO configuration for group 0 and show mapping info.\n");
        printf("      Example: c-func ecx_config_map_group\n");
        return;
    }

    const char *func_name = argv[1];

    /* ------------------------------------------------------------------ */
    /* soem_init <ifname>                                                  */
    /* ------------------------------------------------------------------ */
    if (strcmp(func_name, "soem_init") == 0) {
        if (argc < 3) {
            printf("ERROR: soem_init requires an interface name.\n");
            printf("Usage: c-func soem_init <ifname>\n");
            return;
        }
        const char *ifname = argv[2];
        printf("Calling soem_init(\"%s\")...\n", ifname);
        bool ok = soem_init(ifname);
        printf("Result: %s\n", ok ? "true (success)" : "false (failure)");
    }

    /* ------------------------------------------------------------------ */
    /* soem_cleanup                                                        */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_cleanup") == 0) {
        printf("Calling soem_cleanup()...\n");
        soem_cleanup();
        printf("Result: done\n");
    }

    /* ------------------------------------------------------------------ */
    /* soem_scan_bus                                                       */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_scan_bus") == 0) {
        printf("Calling soem_scan_bus()...\n");
        soem_scan_bus();
    }

    /* ------------------------------------------------------------------ */
    /* soem_request_state <state> [timeout_ms]                            */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_request_state") == 0) {
        if (argc < 3) {
            printf("ERROR: soem_request_state requires a state argument.\n");
            printf("Usage: c-func soem_request_state <state> [timeout_ms]\n");
            printf("  state:      0x01=INIT  0x02=PRE-OP  0x04=SAFE-OP  0x08=OPERATIONAL\n");
            printf("  timeout_ms: milliseconds to wait (default: 5000)\n");
            return;
        }

        uint16_t state      = (uint16_t)strtoul(argv[2], NULL, 0);
        uint32_t timeout_ms = (argc >= 4) ? (uint32_t)strtoul(argv[3], NULL, 0) : 5000u;

        /* Человекочитаемое имя состояния */
        const char *state_name = state_to_string(state);

        printf("Calling soem_request_state(state=0x%02X (%s), timeout_ms=%u)...\n",
               state, state_name, timeout_ms);

        bool ok = soem_request_state(state, timeout_ms);
        printf("Result: %s\n", ok ? "true (all slaves reached state)" : "false (timeout or partial)");
    }

    /* ------------------------------------------------------------------ */
    /* soem_start_pdo                                                      */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_start_pdo") == 0) {
        printf("Calling soem_start_pdo()...\n");
        bool ok = soem_start_pdo();
        printf("Result: %s\n", ok ? "true (PDO exchange started)" : "false (failure)");
    }

    /* ------------------------------------------------------------------ */
    /* soem_stop_pdo                                                       */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_stop_pdo") == 0) {
        printf("Calling soem_stop_pdo()...\n");
        soem_stop_pdo();
        printf("Result: done\n");
    }

    /* ------------------------------------------------------------------ */
    /* soem_exchange_pdo                                                   */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "soem_exchange_pdo") == 0) {
        printf("Calling soem_exchange_pdo()...\n");
        bool ok = soem_exchange_pdo();
        printf("Result: %s\n", ok ? "true (WKC ok)" : "false (WKC mismatch or inactive)");
    }

    /* ------------------------------------------------------------------ */
    /* ecx_config_init                                                     */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "ecx_config_init") == 0) {
        if (!soem_initialized) {
            printf("ERROR: SOEM not initialized. Run 'c-func soem_init <ifname>' first.\n");
            return;
        }

        printf("Calling ecx_config_init(&ecx_context)...\n");

        int result = ecx_config_init(&ecx_context);
        printf("\nConfiguration initialization result:\n");
        printf("  Return code: %d\n", result);
        printf("  Total slaves: %d\n", ecx_context.slavecount);

        /* Show details about each slave */
        printf("\nSlave details:\n");
        for (int i = 1; i <= ecx_context.slavecount; i++) {
            printf("  Slave %d:\n", i);
            printf("    Name: %s\n", ecx_context.slavelist[i].name);
            printf("    Product ID: 0x%08X\n", ecx_context.slavelist[i].eep_id);
            printf("    Vendor ID: 0x%08X\n", ecx_context.slavelist[i].eep_man);
            printf("    Config address: 0x%04X\n", ecx_context.slavelist[i].configadr);
            printf("    Input size: %d bits\n", ecx_context.slavelist[i].Ibits);
            printf("    Output size: %d bits\n", ecx_context.slavelist[i].Obits);
            printf("    State: %s (0x%02X)\n",
                   state_to_string(ecx_context.slavelist[i].state),
                   ecx_context.slavelist[i].state);
        }
    }

    /* ------------------------------------------------------------------ */
    /* ecx_config_map_group                                                */
    /* ------------------------------------------------------------------ */
    else if (strcmp(func_name, "ecx_config_map_group") == 0) {
        if (!soem_initialized) {
            printf("ERROR: SOEM not initialized. Run 'c-func soem_init <ifname>' first.\n");
            return;
        }

        if (ecx_context.slavecount == 0) {
            printf("ERROR: No slaves found. Run 'c-func soem_scan_bus' first.\n");
            return;
        }

        printf("Calling ecx_config_map_group(&ecx_context, (void*)IOmap, 0)...\n");

        int result = ecx_config_map_group(&ecx_context, (void*)IOmap, 0);

        printf("\nPDO Mapping result:\n");
        printf("  Return code: %d (bytes mapped)\n", result);
        printf("  IO map buffer size: %d bytes\n", MAX_IO_MAP_SIZE);

        /* Display PDO group information */
        printf("\nGroup 0 PDO Mapping:\n");
        printf("  Input bytes (Ibytes): %d\n", ecx_context.grouplist[0].Ibytes);
        printf("  Output bytes (Obytes): %d\n", ecx_context.grouplist[0].Obytes);
        printf("  Input WKC: %d\n", ecx_context.grouplist[0].inputsWKC);
        printf("  Output WKC: %d\n", ecx_context.grouplist[0].outputsWKC);
        printf("  Expected WKC: %d\n",
               (ecx_context.grouplist[0].outputsWKC * 2) + ecx_context.grouplist[0].inputsWKC);

        /* Display per-slave PDO information */
        printf("\nPDO mapping per slave:\n");
        for (int i = 1; i <= ecx_context.slavecount; i++) {
            printf("  Slave %d (%s):\n", i, ecx_context.slavelist[i].name);
            printf("    Input offset: %d bytes, %d bits\n",
                   ecx_context.slavelist[i].Ibytes,
                   ecx_context.slavelist[i].Ibits);
            printf("    Output offset: %d bytes, %d bits\n",
                   ecx_context.slavelist[i].Obytes,
                   ecx_context.slavelist[i].Obits);

            /* Show CoE (CANopen over EtherCAT) details if available */
            if (ecx_context.slavelist[i].CoEdetails != 0) {
                printf("    CoE support: yes\n");
            }
        }

        printf("\nTotal IO mapping:\n");
        printf("  Input data size: %d bytes\n", ecx_context.grouplist[0].Ibytes);
        printf("  Output data size: %d bytes\n", ecx_context.grouplist[0].Obytes);
        printf("  Total PDO size: %d bytes\n",
               ecx_context.grouplist[0].Ibytes + ecx_context.grouplist[0].Obytes);
        hex_dump_print((const uint8_t*)&IOmap, result, "IOmap");
    }
    // char *ecx_elist2string(ecx_contextt *context) Look up error in ec_errorlist and convert to text string.
    else if (strcmp(func_name, "ecx_elist2string") == 0) {
        printf(ecx_elist2string(&ecx_context));
    }

    /* ------------------------------------------------------------------ */
    /* Неизвестная функция                                                 */
    /* ------------------------------------------------------------------ */
    else {
        printf("ERROR: Unknown SOEM function '%s'.\n", func_name);
        printf("Type 'c-func' without arguments to see available functions.\n");
    }
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
    else if (strcmp(argv[0], "motor-enable") == 0) {
        cmd_motor_enable(argc, argv);
    }
    else if (strcmp(argv[0], "motor-disable") == 0) {
        cmd_motor_disable(argc, argv);
    }
    else if (strcmp(argv[0], "motor-run") == 0) {
        cmd_motor_run(argc, argv);
    }
    else if (strcmp(argv[0], "motor-velocity") == 0) {
        cmd_motor_velocity(argc, argv);
    }
    else if (strcmp(argv[0], "motor-stop") == 0) {
        cmd_motor_stop(argc, argv);
    }
    else if (strcmp(argv[0], "motor-status") == 0) {
        cmd_motor_status(argc, argv);
    }
    else if (strcmp(argv[0], "c-func") == 0)
    {
        cmd_c_funcs(argc, argv);
    }
    else if (strcmp(argv[0], "history") == 0) {
        if (argc > 1 && strcmp(argv[1], "clear") == 0) {
            history_clear();
        } else {
            history_show();
        }
    }
    else {
        printf("ERROR: Unknown command '%s'. Type 'help' for list of commands.\n", argv[0]);
    }

    return true;
}

/**
 * Главный цикл REPL с поддержкой истории команд
 */
static void repl_loop(void) {
    char line[MAX_COMMAND_LEN];

    /* Загрузить историю при старте */
    history_load();

    printf("\nEtherCAT CLI - Interactive Mode\n");
    printf("Type 'help' for commands, 'quit' to exit\n");
    printf("Use UP/DOWN arrows to navigate command history\n\n");

    while (1) {
        // printf("dummy_says> ");
        // fflush(stdout);

        /* Используем функцию с поддержкой истории */
        if (!read_line_with_history(line, sizeof(line))) {
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

        /* Добавить в историю (и сохранить в файл) */
        history_add(line);

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
    printf("Version 1.1 (SOEM 2.0)\n\n");

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
