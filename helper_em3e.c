#if 0
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
            osal_usleep(100000);

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
#endif
