#ifndef ECAT_ETH_H
#define ECAT_ETH_H

#include <stdint.h>

#define ECAT_SERVER_DEFAULT_PORT 7777
#define ECAT_JSON_MAX_MSG        8192

/*
 * NDJSON Protocol (Newline-Delimited JSON over TCP)
 * -------------------------------------------------
 * Each message is a single JSON object terminated by '\n'.
 * The client sends commands; the server replies with a matching "id" field.
 * The server may also push unsolicited async events (no "id" field).
 *
 * Python -> C  (commands):
 *   {"id":1,  "cmd":"init",      "adapter":"eth0"}
 *   {"id":2,  "cmd":"scan"}
 *   {"id":3,  "cmd":"state",     "target":"op",  "timeout_ms":5000}
 *   {"id":4,  "cmd":"pdo_start"}
 *   {"id":5,  "cmd":"pdo_stop"}
 *   {"id":6,  "cmd":"pdo_read"}
 *   {"id":7,  "cmd":"pdo_write", "hex":"DEADBEEF..."}
 *   {"id":8,  "cmd":"status"}
 *   {"id":9,  "cmd":"cleanup"}
 *   {"id":10, "cmd":"shutdown"}
 *
 * C -> Python  (responses):
 *   {"id":1,  "ok":true}
 *   {"id":2,  "ok":true,  "slaves":3, "io_bytes":100}
 *   {"id":6,  "ok":true,  "hex":"AABBCC..."}
 *   {"id":8,  "ok":true,  "initialized":true, "pdo_running":false,
 *             "slaves":3, "wkc_ok":true}
 *   {"id":3,  "ok":false, "err":"SOEM not initialized"}
 *
 * Async events (pushed by the EtherCAT thread, no "id" field):
 *   {"event":"wkc_error",   "expected":3, "consecutive":10}
 *   {"event":"slave_error", "slave":1,    "state":"SAFE_OP+ERR"}
 */

/*
 * ecat_server_run - Start the TCP/JSON <-> EtherCAT server and block until
 *                   shutdown is requested or a fatal error occurs.
 *
 * @port: TCP port to listen on (use ECAT_SERVER_DEFAULT_PORT if unsure).
 *
 * Returns 0 on clean shutdown, -1 on error.
 */
int ecat_server_run(uint16_t port);

/*
 * ecat_server_stop - Signal the server to shut down gracefully.
 *
 * Safe to call from any thread or a signal handler.
 */
void ecat_server_stop(void);

#endif /* ECAT_ETH_H */
