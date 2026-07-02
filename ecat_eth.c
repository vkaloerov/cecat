/*
 * ecat_eth.c  –  TCP/JSON <-> EtherCAT мастер (прототип)
 *
 * Архитектура потоков:
 *   [Главный поток]   TCP сервер: принимает одно соединение, читает/пишет NDJSON
 *   [EtherCAT поток]  PDO цикл (1 мс) + обработка команд от TCP потока
 *
 * Синхронизация:
 *   - g_shared.mtx     : защищает слот команды/ответа и флаг shutdown
 *   - g_shared.resp_cv : TCP поток ждёт ответа от EtherCAT потока
 *   - g_send_mtx       : защищает запись в сокет клиента
 *
 * Поток данных команды:
 *   TCP recv → parse JSON → lock mtx → post cmd → wait resp_cv
 *   EtherCAT → (в конце PDO цикла) → lock mtx → copy cmd → unlock → execute
 *            → lock mtx → post resp → signal resp_cv → unlock
 *   TCP recv → copy resp → unlock → build JSON → send
 */

/* ============================================================================
 * Платформенные абстракции
 * ============================================================================ */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET           sock_t;
typedef HANDLE           os_thread_t;
typedef CRITICAL_SECTION os_mutex_t;
typedef CONDITION_VARIABLE os_cond_t;
#  define SOCK_INVALID      INVALID_SOCKET
#  define sock_close(s)     closesocket(s)
#  define sock_errno        WSAGetLastError()
#  define os_mutex_init(m)      InitializeCriticalSection(m)
#  define os_mutex_lock(m)      EnterCriticalSection(m)
#  define os_mutex_unlock(m)    LeaveCriticalSection(m)
#  define os_mutex_destroy(m)   DeleteCriticalSection(m)
#  define os_cond_init(c)       InitializeConditionVariable(c)
#  define os_cond_signal(c)     WakeConditionVariable(c)
#  define os_cond_wait(c,m)     SleepConditionVariableCS((c),(m),INFINITE)
#  define os_cond_destroy(c)    ((void)0)
#  define os_sleep_ms(ms)       Sleep(ms)
#  define SEND_FLAGS            0
#  define OS_THREAD_RET         DWORD WINAPI
#  define OS_THREAD_RET_VAL     0
#else
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <errno.h>
#  include <pthread.h>
typedef int              sock_t;
typedef pthread_t        os_thread_t;
typedef pthread_mutex_t  os_mutex_t;
typedef pthread_cond_t   os_cond_t;
#  define SOCK_INVALID      (-1)
#  define sock_close(s)     close(s)
#  define sock_errno        errno
#  define os_mutex_init(m)      pthread_mutex_init((m), NULL)
#  define os_mutex_lock(m)      pthread_mutex_lock(m)
#  define os_mutex_unlock(m)    pthread_mutex_unlock(m)
#  define os_mutex_destroy(m)   pthread_mutex_destroy(m)
#  define os_cond_init(c)       pthread_cond_init((c), NULL)
#  define os_cond_signal(c)     pthread_cond_signal(c)
#  define os_cond_wait(c,m)     pthread_cond_wait((c),(m))
#  define os_cond_destroy(c)    pthread_cond_destroy(c)
#  define os_sleep_ms(ms)       usleep((unsigned int)((ms) * 1000))
#  define SEND_FLAGS            MSG_NOSIGNAL
#  define OS_THREAD_RET         void *
#  define OS_THREAD_RET_VAL     NULL
/* ssize_t is POSIX; Windows uses int for send()/recv() return values */
typedef ssize_t net_ssize_t;
#endif

#ifdef _WIN32
typedef int net_ssize_t;
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#include <cJSON.h>

#include "soem/soem.h"  /* ecx_contextt, EC_STATE_*, ecx_* API */
#include "ecat_eth.h"

/* ============================================================================
 * Константы
 * ============================================================================ */

#define MAX_IO_MAP_SIZE     4096
#define PDO_CYCLE_MS        1       /* Период PDO цикла (мс) */
#define EVENT_QUEUE_SIZE    8       /* Размер кольцевого буфера событий */
#define WKC_ERR_THRESHOLD   10      /* Число подряд WKC-ошибок до события */

/* ============================================================================
 * Типы команд
 * ============================================================================ */

typedef enum {
    ECMD_NONE = 0,
    ECMD_INIT,
    ECMD_CLEANUP,
    ECMD_SCAN,
    ECMD_STATE,
    ECMD_PDO_START,
    ECMD_PDO_STOP,
    ECMD_PDO_READ,
    ECMD_PDO_WRITE,
    ECMD_STATUS,
    ECMD_SHUTDOWN,
    ECMD_CLIENT_CONNECT,    /* internal: client just connected  */
    ECMD_CLIENT_DISCONNECT, /* internal: client just disconnected */
    ECMD_GET_PDO_MAPPING,   /* dynamic PDO field mapping (name/type/offset) */
} ecat_cmd_t;

typedef struct {
    ecat_cmd_t  type;
    int         req_id;
    char        adapter[128];           /* ECMD_INIT  */
    uint16_t    state_target;           /* ECMD_STATE */
    uint32_t    timeout_ms;             /* ECMD_STATE */
    uint8_t     pdo_data[MAX_IO_MAP_SIZE]; /* ECMD_PDO_WRITE */
    int         pdo_data_len;
} ecat_command_t;

typedef struct {
    int      req_id;
    bool     ok;
    char     err[256];
    /* pdo_read */
    uint8_t  pdo_data[MAX_IO_MAP_SIZE];
    int      pdo_data_len;
    /* scan / status */
    int      slave_count;
    int      io_bytes;
    bool     pdo_running;
    bool     wkc_ok;
    /* pdo_mapping: dynamically discovered PDO field layout (owned; handed off
     * to the JSON response object and freed together with it) */
    cJSON    *mapping;
} ecat_response_t;

/* ============================================================================
 * Разделяемое состояние
 * ============================================================================ */

typedef struct {
    os_mutex_t  mtx;
    os_cond_t   resp_cv;        /* TCP ждёт ответа от EtherCAT потока */

    ecat_command_t  cmd;
    bool            cmd_pending;

    ecat_response_t resp;
    bool            resp_ready;

    /* Кольцевой буфер асинхронных событий (JSON строки) */
    char  events[EVENT_QUEUE_SIZE][512];
    int   ev_head;
    int   ev_tail;

    volatile bool shutdown;
    bool          pdo_running;
} shared_t;

static shared_t    g_shared;
static os_mutex_t  g_send_mtx;         /* Защита записи в клиентский сокет */
static sock_t      g_client_sock = SOCK_INVALID;

/* ============================================================================
 * Глобальные переменные SOEM
 * ============================================================================ */

static char          s_IOmap[MAX_IO_MAP_SIZE];
static ecx_contextt  s_ecx_ctx;
static bool          s_initialized = false;
static unsigned int  s_expectedWKC = 0;
static char          s_adapter_name[128] = "";  /* из аргументов командной строки */



/* ============================================================================
 * Утилиты
 * ============================================================================ */

static void srv_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("[ecat-server] ", stdout);
    vprintf(fmt, ap);
    putchar('\n');
    fflush(stdout);
    va_end(ap);
}

static void bin2hex(const uint8_t *data, int len, char *out) {
    for (int i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02X", data[i]);
    out[len * 2] = '\0';
}

static int hex2bin(const char *hex, uint8_t *out, int max_len) {
    int hex_len = (int)strlen(hex);
    if (hex_len % 2 != 0) return -1;
    int nbytes = hex_len / 2;
    if (nbytes > max_len) return -1;
    for (int i = 0; i < nbytes; i++) {
        unsigned int b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return -1;
        out[i] = (uint8_t)b;
    }
    return nbytes;
}

/* Записать строку + '\n' в сокет (thread-safe) */
static bool send_line(sock_t sock, const char *line) {
    if (sock == SOCK_INVALID) return false;
    size_t len = strlen(line);
    /* +1 для '\n', +1 для '\0' */
    char *buf = (char *)malloc(len + 2);
    if (!buf) return false;
    memcpy(buf, line, len);
    buf[len]   = '\n';
    buf[len+1] = '\0';
    os_mutex_lock(&g_send_mtx);
    net_ssize_t sent = send(sock, buf, (int)(len + 1), SEND_FLAGS);
    os_mutex_unlock(&g_send_mtx);
    free(buf);
    return sent > 0;
}

static bool send_json(sock_t sock, cJSON *obj) {
    char *str = cJSON_PrintUnformatted(obj);
    if (!str) return false;
    bool ok = send_line(sock, str);
    cJSON_free(str);
    return ok;
}

static uint16_t parse_state_name(const char *s) {
    if (strcasecmp(s, "init")   == 0) return EC_STATE_INIT;
    if (strcasecmp(s, "preop")  == 0) return EC_STATE_PRE_OP;
    if (strcasecmp(s, "safeop") == 0) return EC_STATE_SAFE_OP;
    if (strcasecmp(s, "op")     == 0) return EC_STATE_OPERATIONAL;
    return (uint16_t)atoi(s);
}

static const char *state_name(uint16_t st) {
    switch (st & 0x0F) {
    case EC_STATE_INIT:         return "INIT";
    case EC_STATE_PRE_OP:       return "PRE_OP";
    case EC_STATE_SAFE_OP:      return "SAFE_OP";
    case EC_STATE_OPERATIONAL:  return "OP";
    default:                    return "UNKNOWN";
    }
}

/* ============================================================================
 * SOEM обёртки
 * ============================================================================ */

static bool srv_soem_init(const char *ifname, char *err, int esz) {
    if (s_initialized) return true;
    if (ecx_init(&s_ecx_ctx, ifname) <= 0) {
        snprintf(err, esz, "ecx_init failed on '%s'", ifname);
        return false;
    }
    s_initialized = true;
    srv_log("SOEM initialized on '%s'", ifname);
    return true;
}

static void srv_soem_cleanup(void) {
    if (s_initialized) {
        ecx_close(&s_ecx_ctx);
        s_initialized = false;
        s_expectedWKC = 0;
        g_shared.pdo_running = false;
        srv_log("SOEM closed");
    }
}

static bool srv_soem_scan(int *slaves_out, int *io_bytes_out, char *err, int esz) {
    if (!s_initialized) { snprintf(err, esz, "SOEM not initialized"); return false; }

    int found = ecx_config_init(&s_ecx_ctx);
    if (found <= 0) { snprintf(err, esz, "No slaves found on bus"); return false; }

    int io_sz = (int)ecx_config_map_group(&s_ecx_ctx, s_IOmap, 0);
    ecx_configdc(&s_ecx_ctx);

    s_expectedWKC = (unsigned int)(s_ecx_ctx.grouplist[0].outputsWKC * 2
                                 + s_ecx_ctx.grouplist[0].inputsWKC);
    *slaves_out   = s_ecx_ctx.slavecount;
    *io_bytes_out = io_sz;
    srv_log("Scan: %d slave(s), IO map %d bytes, WKC expected=%u",
            found, io_sz, s_expectedWKC);
    return true;
}

static bool srv_soem_request_state(uint16_t state, uint32_t timeout_ms,
                                   char *err, int esz) {
    if (!s_initialized) { snprintf(err, esz, "SOEM not initialized"); return false; }
    for (int i = 1; i <= s_ecx_ctx.slavecount; i++) {
        s_ecx_ctx.slavelist[i].state = state;
        ecx_writestate(&s_ecx_ctx, i);
    }
    bool all_ok = true;
    for (int i = 1; i <= s_ecx_ctx.slavecount; i++) {
        uint16_t got = ecx_statecheck(&s_ecx_ctx, i, state,
                                      (int)(timeout_ms * 1000));
        if (got != state) {
            srv_log("Slave %d: wanted %s, got %s", i, state_name(state), state_name(got));
            all_ok = false;
        }
    }
    if (!all_ok && err) snprintf(err, esz, "Not all slaves reached %s", state_name(state));
    return all_ok;
}

static bool srv_soem_pdo_start(char *err, int esz) {
    if (!s_initialized) { snprintf(err, esz, "SOEM not initialized"); return false; }
    if (s_ecx_ctx.slavecount == 0) { snprintf(err, esz, "Run 'scan' first"); return false; }

    char lerr[128] = "";
    bool ok = srv_soem_request_state(EC_STATE_PRE_OP,       5000, lerr, sizeof(lerr))
           && srv_soem_request_state(EC_STATE_SAFE_OP,      5000, lerr, sizeof(lerr))
           && srv_soem_request_state(EC_STATE_OPERATIONAL,  5000, lerr, sizeof(lerr));
    if (!ok && err) snprintf(err, esz, "pdo_start: %s", lerr);
    if (ok) srv_log("PDO started — slaves in OPERATIONAL state");
    return ok;
}

static void srv_soem_pdo_stop(void) {
    char lerr[128] = "";
    srv_soem_request_state(EC_STATE_INIT, 5000, lerr, sizeof(lerr));
    g_shared.pdo_running = false;
    srv_log("PDO stopped");
}

/* Один PDO цикл; возвращает true если WKC совпал
 */
static bool srv_soem_exchange_pdo(void) {
    ecx_send_processdata(&s_ecx_ctx);
    int wkc = ecx_receive_processdata(&s_ecx_ctx, EC_TIMEOUTRET);
    return (unsigned int)wkc >= s_expectedWKC;
}

/* ============================================================================
 * Динамический PDO mapping (аналогично slaveinfo.c: si_map_sii / si_siiPDO)
 *
 * PDO mapping читается напрямую из EEPROM (SII), без обращения к CoE SDO.
 * Вместо печати в stdout строим JSON-описание каждого смапленного поля
 * (имя, index/subindex, абсолютный байт/бит-оффсет внутри s_IOmap, тип и
 * размер), чтобы Python-клиент мог сам паковать/распаковывать поля в
 * бинарном буфере pdo_read/pdo_write, без жёстко зашитых в C-коде структур.
 * ============================================================================ */

/* Строковое имя типа CoE (аналог dtype2string() из slaveinfo.c) */
static const char *srv_dtype2string(uint16_t dtype, uint16_t bitlen) {
    static char str[32];
    switch (dtype) {
    case ECT_BOOLEAN:        snprintf(str, sizeof(str), "BOOLEAN");   break;
    case ECT_INTEGER8:       snprintf(str, sizeof(str), "INTEGER8");  break;
    case ECT_INTEGER16:      snprintf(str, sizeof(str), "INTEGER16"); break;
    case ECT_INTEGER32:      snprintf(str, sizeof(str), "INTEGER32"); break;
    case ECT_INTEGER24:      snprintf(str, sizeof(str), "INTEGER24"); break;
    case ECT_INTEGER64:      snprintf(str, sizeof(str), "INTEGER64"); break;
    case ECT_UNSIGNED8:      snprintf(str, sizeof(str), "UNSIGNED8");  break;
    case ECT_UNSIGNED16:     snprintf(str, sizeof(str), "UNSIGNED16"); break;
    case ECT_UNSIGNED32:     snprintf(str, sizeof(str), "UNSIGNED32"); break;
    case ECT_UNSIGNED24:     snprintf(str, sizeof(str), "UNSIGNED24"); break;
    case ECT_UNSIGNED64:     snprintf(str, sizeof(str), "UNSIGNED64"); break;
    case ECT_REAL32:         snprintf(str, sizeof(str), "REAL32");     break;
    case ECT_REAL64:         snprintf(str, sizeof(str), "REAL64");     break;
    case ECT_BIT1:           snprintf(str, sizeof(str), "BIT1");       break;
    case ECT_BIT2:           snprintf(str, sizeof(str), "BIT2");       break;
    case ECT_BIT3:           snprintf(str, sizeof(str), "BIT3");       break;
    case ECT_BIT4:           snprintf(str, sizeof(str), "BIT4");       break;
    case ECT_BIT5:           snprintf(str, sizeof(str), "BIT5");       break;
    case ECT_BIT6:           snprintf(str, sizeof(str), "BIT6");       break;
    case ECT_BIT7:           snprintf(str, sizeof(str), "BIT7");       break;
    case ECT_BIT8:           snprintf(str, sizeof(str), "BIT8");       break;
    case ECT_VISIBLE_STRING: snprintf(str, sizeof(str), "VISIBLE_STRING(%d)", bitlen); break;
    case ECT_OCTET_STRING:   snprintf(str, sizeof(str), "OCTET_STRING(%d)", bitlen);  break;
    default:                 snprintf(str, sizeof(str), "UNKNOWN(0x%04X,%d)", dtype, bitlen); break;
    }
    return str;
}

/* Определяет знаковость/тип float и формат Python `struct`, которым
 * можно напрямую распаковать байт-выровненное поле (little-endian, как
 * требует EtherCAT). Для бит-полей/24-битных целых/строк возвращает NULL —
 * клиент должен распаковать такие поля вручную по bit_offset/bit_len. */
static void srv_dtype_pyinfo(uint16_t dtype, uint16_t bitlen,
                              bool *out_signed, bool *out_float,
                              const char **out_struct_fmt) {
    *out_signed     = false;
    *out_float      = false;
    *out_struct_fmt = NULL;

    switch (dtype) {
    case ECT_INTEGER8:   *out_signed = true; if (bitlen == 8)  *out_struct_fmt = "<b"; break;
    case ECT_INTEGER16:  *out_signed = true; if (bitlen == 16) *out_struct_fmt = "<h"; break;
    case ECT_INTEGER32:  *out_signed = true; if (bitlen == 32) *out_struct_fmt = "<i"; break;
    case ECT_INTEGER64:  *out_signed = true; if (bitlen == 64) *out_struct_fmt = "<q"; break;
    case ECT_UNSIGNED8:  if (bitlen == 8)  *out_struct_fmt = "<B"; break;
    case ECT_UNSIGNED16: if (bitlen == 16) *out_struct_fmt = "<H"; break;
    case ECT_UNSIGNED32: if (bitlen == 32) *out_struct_fmt = "<I"; break;
    case ECT_UNSIGNED64: if (bitlen == 64) *out_struct_fmt = "<Q"; break;
    case ECT_REAL32:     *out_float  = true; if (bitlen == 32) *out_struct_fmt = "<f"; break;
    case ECT_REAL64:     *out_float  = true; if (bitlen == 64) *out_struct_fmt = "<d"; break;
    case ECT_INTEGER24:  *out_signed = true; break;  /* 3 байта, нет прямого struct-формата */
    case ECT_UNSIGNED24:  break;                       /* 3 байта */
    case ECT_BOOLEAN:
    case ECT_BIT1: case ECT_BIT2: case ECT_BIT3: case ECT_BIT4:
    case ECT_BIT5: case ECT_BIT6: case ECT_BIT7: case ECT_BIT8:
    case ECT_VISIBLE_STRING:
    case ECT_OCTET_STRING:
    default:
        break; /* бит-поле/строка — распаковка вручную на стороне Python */
    }
}

/* Читает PDO mapping слейва напрямую из EEPROM/SII (категория ECT_SII_PDO),
 * без единого SDO-запроса. Направление задаёт `t`: t=1 -> RXPDO (outputs),
 * t=0 -> TXPDO (inputs) — как и в si_siiPDO() из slaveinfo.c. Вместо
 * printf() добавляет по одному JSON-объекту на каждое смапленное (не
 * filler) поле в массив `arr`. Прямой аналог si_siiPDO(); возвращает
 * суммарную битовую длину прочитанных PDO. */
static int srv_siiPDO_to_json(uint16_t slave, uint8_t t, int mapoffset, cJSON *arr) {
    uint16_t a, w, c, e, er;
    uint8_t  eectl;
    uint16_t obj_idx;
    uint8_t  obj_subidx, obj_name, obj_datatype, bitlen;
    int      bitoffset = 0, totalsize = 0;
    ec_eepromPDOt  eepPDO;
    ec_eepromPDOt *PDO = &eepPDO;
    char     str_name[EC_MAXNAME + 1];

    eectl = s_ecx_ctx.slavelist[slave].eep_pdi;

    PDO->nPDO     = 0;
    PDO->Length   = 0;
    PDO->Index[1] = 0;
    for (c = 0; c < EC_MAXSM; c++) PDO->SMbitsize[c] = 0;
    if (t > 1) t = 1;

    PDO->Startpos = ecx_siifind(&s_ecx_ctx, slave, ECT_SII_PDO + t);
    if (PDO->Startpos > 0) {
        a = PDO->Startpos;
        w = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
        w += (ecx_siigetbyte(&s_ecx_ctx, slave, a++) << 8);
        PDO->Length = w;
        c = 1;
        do {
            PDO->nPDO++;
            PDO->Index[PDO->nPDO] = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
            PDO->Index[PDO->nPDO] += (ecx_siigetbyte(&s_ecx_ctx, slave, a++) << 8);
            PDO->BitSize[PDO->nPDO] = 0;
            c++;
            e = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
            PDO->SyncM[PDO->nPDO] = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
            a++;
            obj_name = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
            a += 2;
            c += 2;
            if (PDO->SyncM[PDO->nPDO] < EC_MAXSM) {
                str_name[0] = 0;
                if (obj_name) ecx_siistring(&s_ecx_ctx, str_name, slave, obj_name);

                for (er = 1; er <= e; er++) {
                    c += 4;
                    obj_idx = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
                    obj_idx += (ecx_siigetbyte(&s_ecx_ctx, slave, a++) << 8);
                    obj_subidx = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
                    obj_name = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
                    obj_datatype = ecx_siigetbyte(&s_ecx_ctx, slave, a++);
                    bitlen = ecx_siigetbyte(&s_ecx_ctx, slave, a++);

                    int abs_byte = mapoffset + (bitoffset / 8);
                    int abs_bit  = bitoffset % 8;

                    PDO->BitSize[PDO->nPDO] += bitlen;
                    a += 2;

                    if (obj_idx || obj_subidx) {
                        char fname[EC_MAXNAME + 1] = {0};
                        if (obj_name) ecx_siistring(&s_ecx_ctx, fname, slave, obj_name);

                        bool        is_signed = false, is_float = false;
                        const char *fmt       = NULL;
                        srv_dtype_pyinfo(obj_datatype, bitlen, &is_signed, &is_float, &fmt);
                        int size_bytes = (bitlen + 7) / 8;

                        cJSON *f = cJSON_CreateObject();
                        cJSON_AddStringToObject(f, "name",        fname);
                        cJSON_AddNumberToObject(f, "index",       obj_idx);
                        cJSON_AddNumberToObject(f, "subindex",    obj_subidx);
                        cJSON_AddNumberToObject(f, "pdo_index",   PDO->Index[PDO->nPDO]);
                        cJSON_AddNumberToObject(f, "sm",          PDO->SyncM[PDO->nPDO]);
                        cJSON_AddNumberToObject(f, "byte_offset", abs_byte);
                        cJSON_AddNumberToObject(f, "bit_offset",  abs_bit);
                        cJSON_AddNumberToObject(f, "bit_len",     bitlen);
                        cJSON_AddNumberToObject(f, "size_bytes",  size_bytes);
                        cJSON_AddStringToObject(f, "dtype",       srv_dtype2string(obj_datatype, bitlen));
                        cJSON_AddNumberToObject(f, "dtype_id",    obj_datatype);
                        cJSON_AddBoolToObject(f,   "signed",      is_signed);
                        cJSON_AddBoolToObject(f,   "is_float",    is_float);
                        if (fmt) cJSON_AddStringToObject(f, "struct_fmt", fmt);
                        else     cJSON_AddNullToObject(f, "struct_fmt");

                        cJSON_AddItemToArray(arr, f);
                    }

                    bitoffset += bitlen;
                    totalsize += bitlen;
                }
                PDO->SMbitsize[PDO->SyncM[PDO->nPDO]] += PDO->BitSize[PDO->nPDO];
                c++;
            } else {
                c += 4 * e;
                a += 8 * e;
                c++;
            }
            if (PDO->nPDO >= (EC_MAXEEPDO - 1)) c = PDO->Length;
        } while (c < PDO->Length);
    }
    if (eectl) ecx_eeprom2pdi(&s_ecx_ctx, slave);
    return totalsize;
}

/* Строит полное JSON-описание PDO mapping всех слейвов шины, читая PDO
 * mapping напрямую из EEPROM/SII (аналог si_map_sii() из slaveinfo.c: вызов
 * srv_siiPDO_to_json() сначала для RXPDO (outputs, t=1), потом для TXPDO
 * (inputs, t=0)). Работает только после успешного scan (когда
 * slavelist[].outputs/inputs уже указывают внутрь s_IOmap), поэтому смещения
 * получаются абсолютными в том же буфере, который возвращают
 * pdo_read/принимает pdo_write. */
static bool srv_soem_get_pdo_mapping(cJSON **out, char *err, int esz) {
    if (!s_initialized)              { snprintf(err, esz, "SOEM not initialized"); return false; }
    if (s_ecx_ctx.slavecount == 0)   { snprintf(err, esz, "Run 'scan' first");     return false; }

    cJSON *root       = cJSON_CreateObject();
    cJSON *slaves_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "slaves", slaves_arr);

    for (int slave = 1; slave <= s_ecx_ctx.slavecount; slave++) {
        cJSON *sobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(sobj, "slave", slave);
        cJSON_AddStringToObject(sobj, "name", s_ecx_ctx.slavelist[slave].name);

        cJSON *outputs_arr = cJSON_CreateArray();
        cJSON *inputs_arr  = cJSON_CreateArray();

        int outputs_mapoffset = (int)((uint8_t *)s_ecx_ctx.slavelist[slave].outputs
                                     - (uint8_t *)s_IOmap);
        int inputs_mapoffset  = (int)((uint8_t *)s_ecx_ctx.slavelist[slave].inputs
                                     - (uint8_t *)s_IOmap);

        /* t=1 -> RXPDO (outputs), t=0 -> TXPDO (inputs), как в si_map_sii() */
        int outputs_bits = srv_siiPDO_to_json((uint16_t)slave, 1, outputs_mapoffset, outputs_arr);
        int inputs_bits  = srv_siiPDO_to_json((uint16_t)slave, 0, inputs_mapoffset,  inputs_arr);

        cJSON_AddNumberToObject(sobj, "outputs_bits", outputs_bits);
        cJSON_AddNumberToObject(sobj, "inputs_bits",  inputs_bits);
        cJSON_AddItemToObject(sobj, "outputs", outputs_arr);
        cJSON_AddItemToObject(sobj, "inputs",  inputs_arr);

        cJSON_AddItemToArray(slaves_arr, sobj);
    }

    *out = root;
    return true;
}

/* ============================================================================
 * Очередь асинхронных событий
 * ============================================================================ */

static void push_event(const char *json_str) {
    os_mutex_lock(&g_shared.mtx);
    int next = (g_shared.ev_head + 1) % EVENT_QUEUE_SIZE;
    if (next != g_shared.ev_tail) {
        strncpy(g_shared.events[g_shared.ev_head], json_str,
                sizeof(g_shared.events[0]) - 1);
        g_shared.events[g_shared.ev_head][sizeof(g_shared.events[0]) - 1] = '\0';
        g_shared.ev_head = next;
    }
    os_mutex_unlock(&g_shared.mtx);
}

/* Отправить все накопленные события клиенту (без блокировок кроме g_send_mtx) */
static void flush_events(sock_t client) {
    if (client == SOCK_INVALID) return;
    while (1) {
        char evbuf[512];
        os_mutex_lock(&g_shared.mtx);
        if (g_shared.ev_tail == g_shared.ev_head) {
            os_mutex_unlock(&g_shared.mtx);
            break;
        }
        strncpy(evbuf, g_shared.events[g_shared.ev_tail], sizeof(evbuf) - 1);
        evbuf[sizeof(evbuf) - 1] = '\0';
        g_shared.ev_tail = (g_shared.ev_tail + 1) % EVENT_QUEUE_SIZE;
        os_mutex_unlock(&g_shared.mtx);
        send_line(client, evbuf);
    }
}

/* ============================================================================
 * Обработчик команды (исполняется в потоке EtherCAT)
 * ============================================================================ */

static void ecat_handle_cmd(const ecat_command_t *cmd, ecat_response_t *r) {
    r->req_id       = cmd->req_id;
    r->ok           = false;
    r->err[0]       = '\0';
    r->pdo_data_len = 0;
    r->slave_count  = s_initialized ? s_ecx_ctx.slavecount : 0;
    r->io_bytes     = 0;
    r->pdo_running  = g_shared.pdo_running;
    r->wkc_ok       = true;
    r->mapping      = NULL;


    switch (cmd->type) {

    case ECMD_INIT:
        r->ok = srv_soem_init(cmd->adapter, r->err, sizeof(r->err));
        break;

    case ECMD_CLEANUP:
        srv_soem_cleanup();
        r->ok = true;
        break;

    case ECMD_SCAN:
        r->ok = srv_soem_scan(&r->slave_count, &r->io_bytes, r->err, sizeof(r->err));
        break;

    case ECMD_STATE:
        r->ok = srv_soem_request_state(cmd->state_target, cmd->timeout_ms,
                                       r->err, sizeof(r->err));
        break;

    case ECMD_PDO_START:
        if ((r->ok = srv_soem_pdo_start(r->err, sizeof(r->err))))
            g_shared.pdo_running = true;
        r->pdo_running = g_shared.pdo_running;
        break;

    case ECMD_PDO_STOP:
        srv_soem_pdo_stop();
        r->pdo_running = false;
        r->ok = true;
        break;

    case ECMD_PDO_READ:
        srv_log("PDO READ");
        if (!g_shared.pdo_running) {
            snprintf(r->err, sizeof(r->err), "PDO not running");
        } else {
            memcpy(r->pdo_data, s_IOmap, MAX_IO_MAP_SIZE);
            r->pdo_data_len = MAX_IO_MAP_SIZE;
            r->ok = true;
        }
        break;

    case ECMD_PDO_WRITE:
        srv_log("PDO WRITE");
        if (!g_shared.pdo_running) {
            snprintf(r->err, sizeof(r->err), "PDO not running");
        } else {
            int n = cmd->pdo_data_len < MAX_IO_MAP_SIZE
                  ? cmd->pdo_data_len : MAX_IO_MAP_SIZE;
            memcpy(s_IOmap, cmd->pdo_data, (size_t)n);
            r->ok = true;
        }
        break;

    case ECMD_STATUS:
        r->slave_count = s_initialized ? s_ecx_ctx.slavecount : 0;
        r->pdo_running = g_shared.pdo_running;
        r->ok          = true;
        break;

    case ECMD_SHUTDOWN:
        g_shared.shutdown = true;
        r->ok = true;
        break;

    case ECMD_CLIENT_CONNECT: {
        /* Клиент подключился — автоматически инициализируем SOEM, сканируем шину
         * и переводим слейвы в OPERATIONAL (по аналогии с soem_scan_bus). */
        bool ok = srv_soem_init(cmd->adapter, r->err, sizeof(r->err));
        if (ok) ok = srv_soem_scan(&r->slave_count, &r->io_bytes, r->err, sizeof(r->err));
        if (ok) ok = srv_soem_pdo_start(r->err, sizeof(r->err));
        if (ok) g_shared.pdo_running = true;
        r->ok          = ok;
        r->pdo_running = g_shared.pdo_running;
        if (ok)
            srv_log("EtherCAT bus ready: %d slave(s), IO map %d bytes, OPERATIONAL",
                    r->slave_count, r->io_bytes);
        else
            srv_log("EtherCAT auto-init on client connect failed: %s", r->err);
        break;
    }

    case ECMD_CLIENT_DISCONNECT:
        /* Клиент отключился — корректно останавливаем PDO и закрываем SOEM,
         * т.к. один клиент соответствует ровно одному SOEM-мастеру. */
        if (g_shared.pdo_running) srv_soem_pdo_stop();
        srv_soem_cleanup();
        r->pdo_running = false;
        r->ok = true;
        break;

    case ECMD_GET_PDO_MAPPING:
        r->ok = srv_soem_get_pdo_mapping(&r->mapping, r->err, sizeof(r->err));
        break;

    default:
        snprintf(r->err, sizeof(r->err), "Unknown command type %d", (int)cmd->type);
        break;
    }
}

/* ============================================================================
 * Поток EtherCAT мастера
 * ============================================================================ */

static OS_THREAD_RET ecat_thread_func(void *arg) {
    (void)arg;
    srv_log("EtherCAT thread started (PDO cycle %d ms)", PDO_CYCLE_MS);

    int wkc_err_cnt = 0;

    while (!g_shared.shutdown) {

        /* --- 1. PDO обмен --- */
        if (g_shared.pdo_running) {
            // srv_log("PDO exchange cycle");
            bool ok = srv_soem_exchange_pdo();
            if (!ok) {
                if (++wkc_err_cnt >= WKC_ERR_THRESHOLD) {
                    char evbuf[256];
                    snprintf(evbuf, sizeof(evbuf),
                             "{\"event\":\"wkc_error\","
                             "\"expected\":%u,\"consecutive\":%d}",
                             s_expectedWKC, wkc_err_cnt);
                    push_event(evbuf);
                    wkc_err_cnt = 0;
                }
            } else {
                wkc_err_cnt = 0;
            }
        }

        /* --- 2. Отправить накопленные события --- */
        flush_events(g_client_sock);

        /* --- 3. Проверить входящую команду --- */
        os_mutex_lock(&g_shared.mtx);
        if (g_shared.cmd_pending) {
            ecat_command_t cmd = g_shared.cmd;   /* копия */
            g_shared.cmd_pending = false;
            os_mutex_unlock(&g_shared.mtx);

            ecat_response_t resp;
            memset(&resp, 0, sizeof(resp));
            srv_log("handle_cmd: cmd_id=%d cmd=%d", cmd.req_id, cmd.type);
            ecat_handle_cmd(&cmd, &resp);

            os_mutex_lock(&g_shared.mtx);
            g_shared.resp       = resp;
            g_shared.resp_ready = true;
            os_cond_signal(&g_shared.resp_cv);
            os_mutex_unlock(&g_shared.mtx);
        } else {
            os_mutex_unlock(&g_shared.mtx);
        }

        os_sleep_ms(PDO_CYCLE_MS);
    }

    /* Завершение: остановить PDO и закрыть SOEM */
    if (g_shared.pdo_running) srv_soem_pdo_stop();
    srv_soem_cleanup();

    srv_log("EtherCAT thread stopped");
    return OS_THREAD_RET_VAL;
}

/* ============================================================================
 * Разбор JSON команды
 * ============================================================================ */

static bool parse_json_cmd(cJSON *root, ecat_command_t *cmd,
                           char *err, int esz) {
    memset(cmd, 0, sizeof(*cmd));

    cJSON *jid  = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");

    if (!cJSON_IsNumber(jid))  { snprintf(err, esz, "Missing or invalid 'id'");  return false; }
    if (!cJSON_IsString(jcmd)) { snprintf(err, esz, "Missing or invalid 'cmd'"); return false; }

    cmd->req_id = (int)jid->valuedouble;
    const char *name = jcmd->valuestring;
    srv_log("id=%s cmd=%s", jid->string, jcmd->string);

    if      (!strcmp(name,"init"))      cmd->type = ECMD_INIT;
    else if (!strcmp(name,"cleanup"))   cmd->type = ECMD_CLEANUP;
    else if (!strcmp(name,"scan"))      cmd->type = ECMD_SCAN;
    else if (!strcmp(name,"state"))     cmd->type = ECMD_STATE;
    else if (!strcmp(name,"pdo_start")) cmd->type = ECMD_PDO_START;
    else if (!strcmp(name,"pdo_stop"))  cmd->type = ECMD_PDO_STOP;
    else if (!strcmp(name,"pdo_read"))  cmd->type = ECMD_PDO_READ;
    else if (!strcmp(name,"pdo_write")) cmd->type = ECMD_PDO_WRITE;
    else if (!strcmp(name,"status"))    cmd->type = ECMD_STATUS;
    else if (!strcmp(name,"pdo_mapping")) cmd->type = ECMD_GET_PDO_MAPPING;
    else if (!strcmp(name,"shutdown"))  cmd->type = ECMD_SHUTDOWN;
    else { snprintf(err, esz, "Unknown cmd '%s'", name); return false; }

    if (cmd->type == ECMD_INIT) {
        cJSON *jadp = cJSON_GetObjectItemCaseSensitive(root, "adapter");
        if (!cJSON_IsString(jadp)) {
            snprintf(err, esz, "'init' requires 'adapter' string"); return false;
        }
        strncpy(cmd->adapter, jadp->valuestring, sizeof(cmd->adapter) - 1);
    }

    if (cmd->type == ECMD_STATE) {
        cJSON *jtgt = cJSON_GetObjectItemCaseSensitive(root, "target");
        cJSON *jtmo = cJSON_GetObjectItemCaseSensitive(root, "timeout_ms");
        if (!cJSON_IsString(jtgt)) {
            snprintf(err, esz, "'state' requires 'target' string"); return false;
        }
        cmd->state_target = parse_state_name(jtgt->valuestring);
        cmd->timeout_ms   = cJSON_IsNumber(jtmo) ? (uint32_t)jtmo->valuedouble : 5000;
    }

    if (cmd->type == ECMD_PDO_WRITE) {
        cJSON *jhex = cJSON_GetObjectItemCaseSensitive(root, "hex");
        if (!cJSON_IsString(jhex)) {
            snprintf(err, esz, "'pdo_write' requires 'hex' string"); return false;
        }
        int n = hex2bin(jhex->valuestring, cmd->pdo_data, MAX_IO_MAP_SIZE);
        if (n < 0) { snprintf(err, esz, "Invalid hex in 'pdo_write'"); return false; }
        cmd->pdo_data_len = n;
    }

    return true;
}

/* ============================================================================
 * Сборка JSON ответа
 * ============================================================================ */

static cJSON *build_response(const ecat_response_t *r, ecat_cmd_t ct) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "id", r->req_id);
    cJSON_AddBoolToObject(obj,   "ok", r->ok);

    if (!r->ok) {
        cJSON_AddStringToObject(obj, "err", r->err);
        return obj;
    }

    switch (ct) {
    case ECMD_SCAN:
        cJSON_AddNumberToObject(obj, "slaves",   r->slave_count);
        cJSON_AddNumberToObject(obj, "io_bytes", r->io_bytes);
        break;

    case ECMD_PDO_READ:
        if (r->pdo_data_len > 0) {
            char *hexbuf = (char *)malloc((size_t)r->pdo_data_len * 2 + 1);
            if (hexbuf) {
                bin2hex(r->pdo_data, r->pdo_data_len, hexbuf);
                cJSON_AddStringToObject(obj, "hex", hexbuf);
                free(hexbuf);
            }
        }
        break;

    case ECMD_STATUS:
        cJSON_AddBoolToObject(obj,   "initialized", s_initialized);
        cJSON_AddBoolToObject(obj,   "pdo_running", r->pdo_running);
        cJSON_AddNumberToObject(obj, "slaves",      r->slave_count);
        cJSON_AddBoolToObject(obj,   "wkc_ok",      r->wkc_ok);
        break;

    case ECMD_GET_PDO_MAPPING:
        if (r->mapping) {
            /* Передаём владение объектом — он будет освобождён вместе с `obj`
             * вызывающимом cJSON_Delete(). */
            cJSON_AddItemToObject(obj, "mapping", r->mapping);
        }
        break;

    default:
        break;
    }
    return obj;
}

/* ============================================================================
 * Обслуживание одного TCP клиента
 * ============================================================================ */

static void serve_client(sock_t client) {
    srv_log("Client connected — ready for NDJSON commands");

    char *linebuf = (char *)malloc(ECAT_JSON_MAX_MSG);
    if (!linebuf) { srv_log("OOM"); return; }
    int pos = 0;

    while (!g_shared.shutdown) {
        char ch;
        net_ssize_t n = recv(client, &ch, 1, 0);
        if (n <= 0) {
            if (n == 0) srv_log("Client disconnected");
            else        srv_log("recv error: %d", (int)sock_errno);
            break;
        }

        if (ch == '\r') continue; /* ignore CR from Windows clients */

        if (ch == '\n' || pos >= ECAT_JSON_MAX_MSG - 1) {
            linebuf[pos] = '\0';
            srv_log("line: %s", linebuf);
            pos = 0;
            if (linebuf[0] == '\0') continue; /* пустая строка */

            cJSON *root = cJSON_Parse(linebuf);
            if (!root) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddBoolToObject(e, "ok", false);
                cJSON_AddStringToObject(e, "err", "JSON parse error");
                send_json(client, e);
                cJSON_Delete(e);
                continue;
            }

            char perr[256] = "";
            ecat_command_t cmd;
            bool valid = parse_json_cmd(root, &cmd, perr, sizeof(perr));
            cJSON_Delete(root);

            if (!valid) {
                cJSON *e = cJSON_CreateObject();
                cJSON_AddBoolToObject(e, "ok", false);
                cJSON_AddStringToObject(e, "err", perr);
                send_json(client, e);
                cJSON_Delete(e);
                continue;
            }

            /* Передать команду EtherCAT потоку и ждать ответ */
            os_mutex_lock(&g_shared.mtx);
            srv_log("serve_client parsed cmd: cmd_id=%d cmd=%d", cmd.req_id, cmd.type);
            g_shared.cmd         = cmd;
            g_shared.cmd_pending = true;
            g_shared.resp_ready  = false;
            while (!g_shared.resp_ready && !g_shared.shutdown)
                os_cond_wait(&g_shared.resp_cv, &g_shared.mtx);
            ecat_response_t resp = g_shared.resp;
            g_shared.resp_ready  = false;
            os_mutex_unlock(&g_shared.mtx);

            /* Отправить ответ */
            cJSON *robj = build_response(&resp, cmd.type);
            send_json(client, robj);
            cJSON_Delete(robj);

            if (cmd.type == ECMD_SHUTDOWN) break;

        } else {
            linebuf[pos++] = ch;
        }
    }

    free(linebuf);
    srv_log("Client handler done");
}

/* Отправить внутреннюю команду (не связанную с конкретным клиентским
 * запросом) потоку EtherCAT и дождаться её выполнения. Используется главным
 * потоком при подключении/отключении TCP клиента. */
static void submit_internal_cmd(ecat_cmd_t type, ecat_response_t *resp_out) {
    ecat_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = type;
    cmd.req_id = 0;
    if (type == ECMD_CLIENT_CONNECT)
        strncpy(cmd.adapter, s_adapter_name, sizeof(cmd.adapter) - 1);

    os_mutex_lock(&g_shared.mtx);
    g_shared.cmd         = cmd;
    g_shared.cmd_pending = true;
    g_shared.resp_ready  = false;
    while (!g_shared.resp_ready && !g_shared.shutdown)
        os_cond_wait(&g_shared.resp_cv, &g_shared.mtx);
    if (resp_out) *resp_out = g_shared.resp;
    g_shared.resp_ready = false;
    os_mutex_unlock(&g_shared.mtx);
}

/* ============================================================================
 * Публичный API
 * ============================================================================ */

void ecat_server_stop(void) {
    g_shared.shutdown = true;
    /* Разбудить TCP поток если ждёт ответа */
    os_mutex_lock(&g_shared.mtx);
    os_cond_signal(&g_shared.resp_cv);
    os_mutex_unlock(&g_shared.mtx);
}

int ecat_server_run(const char *bind_addr, uint16_t port, const char *adapter) {
    if (port == 0) port = ECAT_SERVER_DEFAULT_PORT;

    if (!adapter || adapter[0] == '\0') {
        srv_log("EtherCAT adapter name is required");
        return -1;
    }

    /* Инициализация разделяемого состояния */
    memset(&g_shared,   0, sizeof(g_shared));
    memset(&s_ecx_ctx,  0, sizeof(s_ecx_ctx));
    memset(s_IOmap,     0, sizeof(s_IOmap));
    os_mutex_init(&g_shared.mtx);
    os_cond_init(&g_shared.resp_cv);
    os_mutex_init(&g_send_mtx);
    g_client_sock = SOCK_INVALID;

    strncpy(s_adapter_name, adapter, sizeof(s_adapter_name) - 1);
    s_adapter_name[sizeof(s_adapter_name) - 1] = '\0';

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        srv_log("WSAStartup failed");
        return -1;
    }
#endif

    /* Создать серверный сокет */
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == SOCK_INVALID) { srv_log("socket() failed"); return -1; }

    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (!bind_addr || bind_addr[0] == '\0' ||
        strcmp(bind_addr, "any") == 0 || strcmp(bind_addr, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        srv_log("Invalid bind address '%s'", bind_addr); sock_close(srv); return -1;
    }

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        srv_log("bind() failed on %s:%u", bind_addr ? bind_addr : "0.0.0.0", port);
        sock_close(srv); return -1;
    }
    if (listen(srv, 1) < 0) {
        srv_log("listen() failed"); sock_close(srv); return -1;
    }
    srv_log("Listening on %s:%u (one client at a time, adapter '%s')",
            bind_addr ? bind_addr : "0.0.0.0", port, s_adapter_name);

    /* Запустить поток EtherCAT мастера */
    os_thread_t ecat_tid;
#ifdef _WIN32
    ecat_tid = CreateThread(NULL, 0, ecat_thread_func, NULL, 0, NULL);
    if (!ecat_tid) { srv_log("CreateThread failed"); sock_close(srv); return -1; }
#else
    if (pthread_create(&ecat_tid, NULL, ecat_thread_func, NULL) != 0) {
        srv_log("pthread_create failed"); sock_close(srv); return -1;
    }
#endif

    /* Главный цикл: принимаем по одному клиенту */
    while (!g_shared.shutdown) {
        fd_set rfds;
        struct timeval tv = {0, 500000}; /* 500 мс таймаут select */
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);

#ifdef _WIN32
        int ready = select(0, &rfds, NULL, NULL, &tv);
#else
        int ready = select((int)srv + 1, &rfds, NULL, NULL, &tv);
#endif
        if (ready < 0) break;
        if (ready == 0) continue; /* таймаут — проверяем shutdown */

        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        sock_t client = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
        if (client == SOCK_INVALID) {
            if (g_shared.shutdown) break;
            srv_log("accept() error: %d", (int)sock_errno);
            continue;
        }
        srv_log("Connection from %s:%d",
                inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));

        /* Клиент и SOEM мастер связаны 1-к-1: автоматически инициализируем
         * и сканируем шину перед тем, как отдать управление клиенту. */
        ecat_response_t connect_resp;
        memset(&connect_resp, 0, sizeof(connect_resp));
        submit_internal_cmd(ECMD_CLIENT_CONNECT, &connect_resp);
        if (!connect_resp.ok) {
            srv_log("Warning: EtherCAT auto-init failed for this client: %s",
                    connect_resp.err);
        }

        os_mutex_lock(&g_shared.mtx);
        g_client_sock = client;
        os_mutex_unlock(&g_shared.mtx);

        serve_client(client);

        os_mutex_lock(&g_shared.mtx);
        g_client_sock = SOCK_INVALID;
        os_mutex_unlock(&g_shared.mtx);

        sock_close(client);

        /* Клиент отключился — корректно завершаем его SOEM-сессию. */
        submit_internal_cmd(ECMD_CLIENT_DISCONNECT, NULL);
    }

    /* Завершение */
    g_shared.shutdown = true;
    sock_close(srv);

#ifdef _WIN32
    WaitForSingleObject(ecat_tid, 10000);
    CloseHandle(ecat_tid);
    WSACleanup();
#else
    pthread_join(ecat_tid, NULL);
#endif

    os_cond_destroy(&g_shared.resp_cv);
    os_mutex_destroy(&g_shared.mtx);
    os_mutex_destroy(&g_send_mtx);

    srv_log("Server shut down cleanly");
    return 0;
}
