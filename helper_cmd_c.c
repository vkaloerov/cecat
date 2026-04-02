#include "helper_cmd_c.h"

/* ── Standard library ─────────────────────────────────────────────────────── */
#include <stdio.h>      /* printf                                              */
#include <stdlib.h>     /* strtoul                                             */
#include <string.h>     /* strcmp                                              */
#include <stdint.h>     /* uint8_t, uint16_t, uint32_t                        */
#include <stdbool.h>    /* bool, true, false                                   */

/* ── Project headers ──────────────────────────────────────────────────────── */
#include "ecat_cli.h"       /* shared globals, SOEM wrappers, state_to_string */
#include "my_hex_dump.h"    /* hex_dump_print                                  */
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

void cmd_c_funcs(int argc, char **argv) {
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
        /* NOTE: never pass an untrusted string as a printf format argument.
         *       ecx_elist2string() may contain '%' characters from device names. */
        printf("%s\n", ecx_elist2string(&ecx_context));
    }

    /* ------------------------------------------------------------------ */
    /* Неизвестная функция                                                 */
    /* ------------------------------------------------------------------ */
    else {
        printf("ERROR: Unknown SOEM function '%s'.\n", func_name);
        printf("Type 'c-func' without arguments to see available functions.\n");
    }
}
