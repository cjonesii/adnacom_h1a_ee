#ifndef __EEP_H__
#define __EEP_H__

#include <stdbool.h>
#include <stdint.h>

#define EEP_STAT_N_CTRL_ADDR    (0x260)
#define EEP_BUFFER_ADDR         (0x264)
#define EEP_CLK_FREQ_ADDR       (0x268)
#define EEP_3RD_ADDR_BYTE_ADDR  (0x26C)

/* Serial EEPROM Status and Control (260h) */
/* Serial EEPROM Control */
#define EEP_BLKADDR_OFFSET      (0)             /* [12:0] */
#define EEP_CMD_OFFSET          (13)            /* [15:13] */
/* Serial EEPROM Status */
#define EEP_PRSNT_OFFSET        (16)            /* [17:16] */
#define EEP_CMD_STATUS_OFFSET   (18)
#define EEP_BLK_ADDR_UPPER_OFFSET       (20)
#define EEP_ADDR_WIDTH_OVERRIDE_OFFSET  (21)
#define EEP_ADDR_WIDTH_OFFSET   (22)            /* [23:22] */
/* Serial EEPROM Status Data */
#define EEP_RDY_OFFSET          (24)
#define EEP_WR_ENABLE_OFFSET    (25)
#define EEP_BLK_WR_PROTECT_OFFSET       (26)    /* [27:26] */
#define EEP_WR_STATUS_OFFSET    (28)            /* [30:28] */
#define EEP_WR_PROTECT_EN_OFFSET        (31)

#define EEP_INIT_VAL            (0x0000005A)
#define PCI_MEM_ERROR           (0xFFFFFFFF)

#define CMD_LINE_ERR      2
#define EEP_FAIL          3
#define EEP_NOT_EXIST     4
#define EEP_BLANK_INVALID 5
#define EEP_WIDTH_ERROR   6

enum EEP_CMD {
    RSVD_000_CMD,
    WR_REG_STAT_DATA_TO_EEP,
    WR_4B_FR_BUFF_TO_BLKADDR,
    RD_4B_FR_BLKADDR_TO_BUFF,
    RST_WR_EN_LATCH,
    WR_EEP_STAT_DATA_TO_REG,
    SET_WR_EN_LATCH,
    RSV_111_CMD
};

enum EEP_PRSNT {
    NOT_PRSNT,
    PRSNT_VALID,
    RSVD_PRSNT,
    PRSNT_INVALID,
    EEP_PRSNT_MAX
};

enum EEP_CMD_STAT {
    CMD_COMPLETE,
    CMD_NOT_COMPLETE,
    EEP_CMD_STAT_MAX
};

enum EEP_ADDR_WIDTH_OVERRIDE {
    ADDR_WIDTH_RO,
    ADDR_WIDTH_WRITABLE,
    EEP_ADDR_WIDTH_OVERRIDE_MAX
};

enum EEP_ADDR_WIDTH {
    UNDERTERMINED,
    ONE_BYTE,
    TWO_BYTES,
    THREE_BYTES,
    EEP_ADDR_WIDTH_MAX
};

enum EEP_READY {
    EEP_READY_TO_TX,
    EEP_WR_ONGOING,
    EEP_READY_MAX
};

enum EEP_WR_EN {
    EEP_WR_DISABLED,
    EEP_WR_ENABLED,
    EEP_WR_EN_MAX
};

union eep_status_and_control_reg {
    struct _cmd_n_status_struct {
        uint32_t blk_addr             : 13;/* [12:0] */
        uint32_t cmd                  : 3; /* [15:13] */
        uint32_t prsnt                : 2; /* [17:16] */
        uint32_t cmd_status           : 1; /* [18] */
        uint32_t rsvd_bit19           : 1; /* [19] */
        uint32_t blk_upper_bit        : 1; /* [20] */
        uint32_t addr_width_override  : 1; /* [21] */
        uint32_t addr_width           : 2; /* [23:22] */
        uint32_t ready                : 1; /* [24] */
        uint32_t write_enable         : 1; /* [25] */
        uint32_t block_protection     : 2; /* [27:26] */
        uint32_t write_status         : 3; /* [30:28] */
        uint32_t write_protect_enable : 1; /* [31] */
    } cmd_n_status_struct;
    uint32_t cmd_u32;
};

enum access {
    REG_WRITE,
    REG_READ
};

#endif // __EEP_H__