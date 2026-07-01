/*
 * ecat_server_main.c – Entry point for ecat-server
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifndef _WIN32
#  include <unistd.h>
#endif
#include "ecat_eth.h"

static void signal_handler(int sig)
{
    (void)sig;
    ecat_server_stop();
}

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [-p <port>] [-h]\n\n", prog_name);
    printf("Options:\n");
    printf("  -p, --port <port>  TCP port to listen on (default: %d)\n",
           ECAT_SERVER_DEFAULT_PORT);
    printf("  -h, --help         Show this help message and exit\n\n");
    printf("NDJSON Protocol (one JSON object per line over TCP)\n");
    printf("---------------------------------------------------\n\n");
    printf("Python -> C (commands):\n");
    printf("  {\"id\":1,  \"cmd\":\"init\",      \"adapter\":\"eth0\"}\n");
    printf("  {\"id\":2,  \"cmd\":\"scan\"}\n");
    printf("  {\"id\":3,  \"cmd\":\"state\",     \"target\":\"op\", \"timeout_ms\":5000}\n");
    printf("  {\"id\":4,  \"cmd\":\"pdo_start\"}\n");
    printf("  {\"id\":5,  \"cmd\":\"pdo_stop\"}\n");
    printf("  {\"id\":6,  \"cmd\":\"pdo_read\"}\n");
    printf("  {\"id\":7,  \"cmd\":\"pdo_write\", \"hex\":\"DEADBEEF...\"}\n");
    printf("  {\"id\":8,  \"cmd\":\"status\"}\n");
    printf("  {\"id\":9,  \"cmd\":\"cleanup\"}\n");
    printf("  {\"id\":10, \"cmd\":\"shutdown\"}\n\n");
    printf("C -> Python (responses):\n");
    printf("  {\"id\":1, \"ok\":true}\n");
    printf("  {\"id\":2, \"ok\":true,  \"slaves\":3, \"io_bytes\":100}\n");
    printf("  {\"id\":6, \"ok\":true,  \"hex\":\"AABBCC...\"}\n");
    printf("  {\"id\":8, \"ok\":true,  \"initialized\":true, \"pdo_running\":false,\n");
    printf("            \"slaves\":3, \"wkc_ok\":true}\n");
    printf("  {\"id\":3, \"ok\":false, \"err\":\"SOEM not initialized\"}\n\n");
    printf("Async events (pushed by EtherCAT thread, no \"id\" field):\n");
    printf("  {\"event\":\"wkc_error\",   \"expected\":3, \"consecutive\":10}\n");
    printf("  {\"event\":\"slave_error\", \"slave\":1,    \"state\":\"SAFE_OP+ERR\"}\n");
}

int main(int argc, char *argv[])
{
    uint16_t port = ECAT_SERVER_DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", argv[i]);
                return EXIT_FAILURE;
            }
            int p = atoi(argv[++i]);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "Error: invalid port number: %s\n", argv[i]);
                return EXIT_FAILURE;
            }
            port = (uint16_t)p;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

#ifdef _WIN32
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
#else
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
#endif

    printf("Starting ecat-server on port %u\n", (unsigned)port);
    int ret = ecat_server_run(port);
    if (ret != 0) {
        fprintf(stderr, "ecat_server_run failed\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
