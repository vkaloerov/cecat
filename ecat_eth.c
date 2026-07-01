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

/* Один PDO цикл; возвращает true если WKC совпал */
static bool srv_soem_exchange_pdo(void) {
    ecx_send_processdata(&s_ecx_ctx);
    int wkc = ecx_receive_processdata(&s_ecx_ctx, EC_TIMEOUTRET);
    return (unsigned int)wkc >= s_expectedWKC;
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
        if (!g_shared.pdo_running) {
            snprintf(r->err, sizeof(r->err), "PDO not running");
        } else {
            memcpy(r->pdo_data, s_IOmap, MAX_IO_MAP_SIZE);
            r->pdo_data_len = MAX_IO_MAP_SIZE;
            r->ok = true;
        }
        break;

    case ECMD_PDO_WRITE:
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

    if      (!strcmp(name,"init"))      cmd->type = ECMD_INIT;
    else if (!strcmp(name,"cleanup"))   cmd->type = ECMD_CLEANUP;
    else if (!strcmp(name,"scan"))      cmd->type = ECMD_SCAN;
    else if (!strcmp(name,"state"))     cmd->type = ECMD_STATE;
    else if (!strcmp(name,"pdo_start")) cmd->type = ECMD_PDO_START;
    else if (!strcmp(name,"pdo_stop"))  cmd->type = ECMD_PDO_STOP;
    else if (!strcmp(name,"pdo_read"))  cmd->type = ECMD_PDO_READ;
    else if (!strcmp(name,"pdo_write")) cmd->type = ECMD_PDO_WRITE;
    else if (!strcmp(name,"status"))    cmd->type = ECMD_STATUS;
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

int ecat_server_run(uint16_t port) {
    if (port == 0) port = ECAT_SERVER_DEFAULT_PORT;

    /* Инициализация разделяемого состояния */
    memset(&g_shared,   0, sizeof(g_shared));
    memset(&s_ecx_ctx,  0, sizeof(s_ecx_ctx));
    memset(s_IOmap,     0, sizeof(s_IOmap));
    os_mutex_init(&g_shared.mtx);
    os_cond_init(&g_shared.resp_cv);
    os_mutex_init(&g_send_mtx);
    g_client_sock = SOCK_INVALID;

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
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        srv_log("bind() failed on port %u", port); sock_close(srv); return -1;
    }
    if (listen(srv, 1) < 0) {
        srv_log("listen() failed"); sock_close(srv); return -1;
    }
    srv_log("Listening on port %u (one client at a time)", port);

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

        os_mutex_lock(&g_shared.mtx);
        g_client_sock = client;
        os_mutex_unlock(&g_shared.mtx);

        serve_client(client);

        os_mutex_lock(&g_shared.mtx);
        g_client_sock = SOCK_INVALID;
        os_mutex_unlock(&g_shared.mtx);

        sock_close(client);
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
