/*
 * list_adapters.c - Утилита для диагностики сетевых интерфейсов
 * 
 * Показывает доступные сетевые адаптеры для работы с SOEM/Npcap
 * Помогает определить правильное имя интерфейса для использования
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#include <pcap.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wpcap.lib")
#endif

#include "soem/soem.h"

void print_adapters_pcap() {
    pcap_if_t *alldevs;
    pcap_if_t *d;
    char errbuf[PCAP_ERRBUF_SIZE];
    int i = 0;

    printf("\n=== Npcap/WinPcap Adapters ===\n\n");

    /* Получаем список устройств */
    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        fprintf(stderr, "Error in pcap_findalldevs: %s\n", errbuf);
        return;
    }

    /* Печатаем список */
    for (d = alldevs; d; d = d->next) {
        printf("%d. %s\n", ++i, d->name);
        if (d->description) {
            printf("   Description: %s\n", d->description);
        } else {
            printf("   (No description available)\n");
        }

        /* Печатаем адреса */
        pcap_addr_t *a;
        for (a = d->addresses; a; a = a->next) {
            if (a->addr && a->addr->sa_family == AF_INET) {
                struct sockaddr_in *addr_in = (struct sockaddr_in *)a->addr;
                printf("   IP: %s\n", inet_ntoa(addr_in->sin_addr));
            }
        }

        printf("   Flags:");
        if (d->flags & PCAP_IF_LOOPBACK)
            printf(" LOOPBACK");
        if (d->flags & PCAP_IF_UP)
            printf(" UP");
        if (d->flags & PCAP_IF_RUNNING)
            printf(" RUNNING");
        printf("\n\n");
    }

    if (i == 0) {
        printf("No interfaces found!\n");
        printf("\nPossible reasons:\n");
        printf("  1. Npcap is not installed\n");
        printf("  2. Application is not running with administrator privileges\n");
        printf("  3. Npcap is installed in WinPcap compatibility mode is disabled\n");
    }

    pcap_freealldevs(alldevs);
}

void print_adapters_windows() {
#ifdef _WIN32
    PIP_ADAPTER_ADDRESSES pAddresses = NULL;
    ULONG outBufLen = 15000;
    DWORD dwRetVal = 0;
    ULONG iterations = 0;

    printf("\n=== Windows Network Adapters ===\n\n");

    do {
        pAddresses = (IP_ADAPTER_ADDRESSES *)malloc(outBufLen);
        if (pAddresses == NULL) {
            printf("Memory allocation failed\n");
            return;
        }

        dwRetVal = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_INCLUDE_PREFIX,
            NULL,
            pAddresses,
            &outBufLen);

        if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
            free(pAddresses);
            pAddresses = NULL;
        } else {
            break;
        }

        iterations++;
    } while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (iterations < 3));

    if (dwRetVal == NO_ERROR) {
        PIP_ADAPTER_ADDRESSES pCurrAddresses = pAddresses;
        int i = 1;

        while (pCurrAddresses) {
            printf("%d. %S\n", i++, pCurrAddresses->FriendlyName);
            printf("   Description: %S\n", pCurrAddresses->Description);
            printf("   Adapter name: %s\n", pCurrAddresses->AdapterName);
            
            /* Показываем MAC адрес */
            if (pCurrAddresses->PhysicalAddressLength != 0) {
                printf("   MAC Address: ");
                for (DWORD j = 0; j < pCurrAddresses->PhysicalAddressLength; j++) {
                    if (j == (pCurrAddresses->PhysicalAddressLength - 1))
                        printf("%.2X\n", (int)pCurrAddresses->PhysicalAddress[j]);
                    else
                        printf("%.2X-", (int)pCurrAddresses->PhysicalAddress[j]);
                }
            }

            /* Показываем IP адреса */
            PIP_ADAPTER_UNICAST_ADDRESS pUnicast = pCurrAddresses->FirstUnicastAddress;
            if (pUnicast != NULL) {
                for (; pUnicast != NULL; pUnicast = pUnicast->Next) {
                    if (pUnicast->Address.lpSockaddr->sa_family == AF_INET) {
                        struct sockaddr_in *sa_in = (struct sockaddr_in *)pUnicast->Address.lpSockaddr;
                        printf("   IPv4: %s\n", inet_ntoa(sa_in->sin_addr));
                    }
                }
            }

            printf("   Status: %s\n",
                pCurrAddresses->OperStatus == IfOperStatusUp ? "UP" : "DOWN");
            printf("   NPF Device: \\Device\\NPF_{%s}\n", pCurrAddresses->AdapterName);
            printf("\n");

            pCurrAddresses = pCurrAddresses->Next;
        }
    } else {
        printf("GetAdaptersAddresses failed with error: %d\n", dwRetVal);
    }

    if (pAddresses) {
        free(pAddresses);
    }
#endif
}

void test_soem_init(const char *ifname) {
    ecx_contextt ecx_context;
    
    printf("\n=== Testing SOEM Init ===\n");
    printf("Interface: %s\n", ifname);
    
    int result = ecx_init(&ecx_context, ifname);
    if (result > 0) {
        printf("✓ SUCCESS! SOEM initialized successfully\n");
        printf("  Result: %d\n", result);
        ecx_close(&ecx_context);
    } else {
        printf("✗ FAILED! ecx_init returned: %d\n", result);
        printf("\nPossible solutions:\n");
        printf("  1. Run as Administrator\n");
        printf("  2. Check if Npcap is installed: https://npcap.com/#download\n");
        printf("  3. Install Npcap with 'WinPcap API-compatible Mode' enabled\n");
        printf("  4. Check if the interface is UP and RUNNING\n");
        printf("  5. Disable antivirus/firewall temporarily\n");
        printf("  6. Try reinstalling Npcap\n");
    }
}

void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("\nOptions:\n");
    printf("  -t, --test <interface>  Test SOEM initialization with specified interface\n");
    printf("  -h, --help              Show this help message\n");
    printf("\nExample:\n");
    printf("  %s\n", prog_name);
    printf("  %s -t \"\\Device\\NPF_{E0FF3CC3-015D-401E-9F41-6C525F9D4DB9}\"\n", prog_name);
}

int main(int argc, char *argv[]) {
    const char *test_interface = NULL;

    printf("=== Network Adapter Diagnostic Tool ===\n");
    printf("Version 1.0\n");

#ifdef _WIN32
    /* Инициализация Winsock */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    /* Парсинг аргументов */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            if (i + 1 < argc) {
                test_interface = argv[++i];
            } else {
                printf("ERROR: -t option requires an argument\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            printf("ERROR: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    /* Показываем адаптеры через Windows API */
    print_adapters_windows();

    /* Показываем адаптеры через Pcap */
    print_adapters_pcap();

    /* Если указан интерфейс для теста - тестируем */
    if (test_interface) {
        test_soem_init(test_interface);
    } else {
        printf("\n=== Recommendations ===\n");
        printf("1. Choose an interface that is UP and RUNNING\n");
        printf("2. Use the NPF Device path with your application\n");
        printf("3. Run with Administrator privileges\n");
        printf("4. Test the interface with: %s -t \"<NPF_Device_Path>\"\n", argv[0]);
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return 0;
}