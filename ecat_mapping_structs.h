#ifndef ECAT_MAPPING_STRUCTS_H
#define ECAT_MAPPING_STRUCTS_H

#include <stdint.h>
#include "soem/soem.h"

OSAL_PACKED_BEGIN
typedef struct OSAL_PACKED
{
    struct  OSAL_PACKED {
        uint8_t _1_display; /* addr 0x0000, slave DC_Device */
        uint8_t _2_display; /* addr 0x0001, slave DC_Device */
        uint8_t _3_display; /* addr 0x0002, slave DC_Device */
        uint8_t _4_display; /* addr 0x0003, slave DC_Device */
        uint8_t _5_display; /* addr 0x0004, slave DC_Device */
        uint8_t _6_display; /* addr 0x0005, slave DC_Device */
        uint8_t _7_display; /* addr 0x0006, slave DC_Device */
        uint8_t _8_display; /* addr 0x0007, slave DC_Device */
        uint8_t _9_display; /* addr 0x0008, slave DC_Device */
        uint8_t _10_display; /* addr 0x0009, slave DC_Device */
        uint8_t _11_display; /* addr 0x000A, slave DC_Device */
        uint8_t _12_display; /* addr 0x000B, slave DC_Device */
        uint8_t _13_display; /* addr 0x000C, slave DC_Device */
        uint8_t _14_display; /* addr 0x000D, slave DC_Device */
        uint8_t _15_display; /* addr 0x000E, slave DC_Device */
        uint8_t _16_display; /* addr 0x000F, slave DC_Device */
        uint32_t Target_pos; /* addr 0x0010, slave DC_Device */
        uint32_t Max_speed; /* addr 0x0014, slave DC_Device */
        uint32_t Relays; /* addr 0x0018, slave DC_Device */
        uint32_t Control_flags; /* addr 0x0018, slave DC_Device */
    } outputs;
    struct  OSAL_PACKED {
        uint16_t Dbg_fixed; /* addr 0x001C, slave DC_Device */
        uint16_t Dbg_counter; /* addr 0x001E, slave DC_Device */
        uint32_t Enc0; /* addr 0x0020, slave DC_Device */
        uint32_t Enc1; /* addr 0x0024, slave DC_Device */
        uint32_t Enc2; /* addr 0x0028, slave DC_Device */
        uint32_t Enc3; /* addr 0x002C, slave DC_Device */
        uint32_t Buttons; /* addr 0x0030, slave DC_Device */
        uint32_t Flags; /* addr 0x0034, slave DC_Device */
        uint32_t Enc_abs; /* addr 0x0038, slave DC_Device */
        uint32_t Enc_quad; /* addr 0x003C, slave DC_Device */
        uint32_t Limiters; /* addr 0x0040, slave DC_Device */
        uint32_t Motor_pos; /* addr 0x0044, slave DC_Device */
        int32_t Motor_speed; /* addr 0x0048, slave DC_Device */
    } inputs;
} process_data_t;
OSAL_PACKED_END

#endif
