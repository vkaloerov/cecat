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
 *   {"id":11, "cmd":"pdo_mapping"}
 *
 * C -> Python  (responses):
 *   {"id":1,  "ok":true}
 *   {"id":2,  "ok":true,  "slaves":3, "io_bytes":100}
 *   {"id":6,  "ok":true,  "hex":"AABBCC..."}
 *   {"id":8,  "ok":true,  "initialized":true, "pdo_running":false,
 *             "slaves":3, "wkc_ok":true}
 *   {"id":3,  "ok":false, "err":"SOEM not initialized"}
 *
 *   {"id":11, "ok":true, "mapping": {
 *       "slaves": [
 *         { "slave":1, "name":"EM3E-556",
 *           "outputs_bits":128, "inputs_bits":256,
 *           "outputs": [
 *             { "name":"Target_pos", "index":24674, "subindex":1,
 *               "byte_offset":0, "bit_offset":0, "bit_len":32,
 *               "size_bytes":4, "dtype":"INTEGER32", "dtype_id":792,
 *               "signed":true, "is_float":false, "struct_fmt":"<i" },
 *             ...
 *           ],
 *           "inputs": [ ... ]
 *         }, ...
 *       ]
 *   }}
 *
 * The "pdo_mapping" command dynamically walks each slave's CoE PDO assign
 * objects (0x1c10..0x1c13) via SDO reads (see srv_soem_get_pdo_mapping() /
 * srv_pdo_assign_to_json() in ecat_eth.c, modeled after si_map_sdo() /
 * si_PDOassign() in SOEM's slaveinfo.c sample) so the layout does not need to
 * be hardcoded on the C side. "byte_offset"/"bit_offset" are absolute
 * positions inside the same buffer returned by "pdo_read" / expected by
 * "pdo_write". The C server never interprets field values itself: encoding
 * and decoding of individual fields (using "struct_fmt", or manual bit
 * extraction for sub-byte/24-bit/string fields) is entirely up to the
 * client (see PdoImage in ecat_client.py).
 *
 * Async events (pushed by the EtherCAT thread, no "id" field):
 *   {"event":"wkc_error",   "expected":3, "consecutive":10}
 *   {"event":"slave_error", "slave":1,    "state":"SAFE_OP+ERR"}
 */

/*
 * ecat_server_run - Start the TCP/JSON <-> EtherCAT server and block until
 *                   shutdown is requested or a fatal error occurs.
 *
 * The server accepts exactly one TCP client at a time. The lifetime of the
 * SOEM EtherCAT master session is tied 1:1 to that client's connection: the
 * master is initialized and the bus is scanned/configured up to the
 * OPERATIONAL state automatically as soon as a client connects, and it is
 * cleanly stopped (PDO stop + SOEM close) automatically as soon as that
 * client disconnects, so the next client starts from a clean state.
 *
 * @bind_addr: IPv4 address to listen on (e.g. "0.0.0.0" or "192.168.0.10").
 *             NULL or an empty string binds to all interfaces (INADDR_ANY).
 * @port:      TCP port to listen on (use ECAT_SERVER_DEFAULT_PORT if unsure).
 * @adapter:   Network adapter/interface name used for the SOEM EtherCAT
 *             master (e.g. "eth0", or "\\Device\\NPF_{...}" on Windows).
 *             Required (must not be NULL/empty).
 *
 * Returns 0 on clean shutdown, -1 on error.
 */
int ecat_server_run(const char *bind_addr, uint16_t port, const char *adapter);

/*
 * ecat_server_stop - Signal the server to shut down gracefully.
 *
 * Safe to call from any thread or a signal handler.
 */
void ecat_server_stop(void);

#endif /* ECAT_ETH_H */
