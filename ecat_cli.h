#ifndef ECAT_CLI_H
#define ECAT_CLI_H

/*
 * ecat_cli.h - Public interface of ecat_cli.c
 *
 * Exports:
 *   - Shared constants  (MAX_IO_MAP_SIZE)
 *   - Shared globals    (IOmap, ecx_context, soem_initialized, expectedWKC)
 *   - SOEM wrapper API  (soem_init / cleanup / scan / state / PDO)
 *   - Utility functions (state_to_string)
 *
 * Include this header in any translation unit that needs to call SOEM wrapper
 * functions or inspect the shared EtherCAT state (e.g. helper_cmd_c.c).
 */

#include <stdint.h>
#include <stdbool.h>

#include "soem/soem.h"   /* ecx_contextt, EC_STATE_*, ecx_* API */

/* ============================================================================
 * Constants
 * ============================================================================ */

/** Size of the shared PDO I/O map buffer in bytes. */
#define MAX_IO_MAP_SIZE 4096

/* ============================================================================
 * Shared globals  (defined in ecat_cli.c)
 * ============================================================================ */

/** Raw PDO I/O map buffer.  Inputs start at offset 0; outputs follow. */
extern char          IOmap[MAX_IO_MAP_SIZE];

/** SOEM 2.0 context structure — passed to every ecx_* call. */
extern ecx_contextt  ecx_context;

/** True once ecx_init() has succeeded and SOEM owns the NIC. */
extern bool          soem_initialized;

/** Expected working counter: (outputsWKC * 2) + inputsWKC. */
extern unsigned int  expectedWKC;

/* ============================================================================
 * SOEM wrapper functions  (defined in ecat_cli.c)
 * ============================================================================ */

/**
 * Initialise SOEM on the given network interface.
 *
 * @param ifname  Interface name, e.g. "eth0" or a WinPcap adapter GUID.
 * @return true on success, false if ecx_init() fails.
 */
bool soem_init(const char *ifname);

/**
 * Close the SOEM context and release the NIC.
 * Safe to call even when SOEM is not initialised (no-op).
 */
void soem_cleanup(void);

/**
 * Scan the EtherCAT bus and print information about every discovered slave.
 * Requires soem_init() to have been called first.
 */
void soem_scan_bus(void);

/**
 * Ask every slave to transition to @p state and wait up to @p timeout_ms ms.
 *
 * @param state       Target EC state (EC_STATE_INIT / PRE_OP / SAFE_OP / OP …).
 * @param timeout_ms  Per-slave timeout in milliseconds (0 → use 5000).
 * @return true if all slaves acknowledged the state, false on timeout/partial.
 */
bool soem_request_state(uint16_t state, uint32_t timeout_ms);

/**
 * Configure PDO mapping and drive all slaves to OPERATIONAL state.
 *
 * @return true on success.
 */
bool soem_start_pdo(void);

/**
 * Drive all slaves back to INIT and tear down PDO exchange.
 */
void soem_stop_pdo(void);

/**
 * Perform one PDO send/receive cycle (ecx_send_processdata + ecx_receive_processdata).
 *
 * @return true if the working counter matches expectedWKC, false otherwise.
 */
bool soem_exchange_pdo(void);

/* ============================================================================
 * Utility functions  (defined in ecat_cli.c)
 * ============================================================================ */

/**
 * Convert a numeric EtherCAT slave state to a human-readable string.
 *
 * @param state  Raw state value (0x01 = INIT, 0x02 = PRE-OP, …).
 * @return Pointer to a static string; never NULL.
 */
const char *state_to_string(uint16_t state);

#endif /* ECAT_CLI_H */