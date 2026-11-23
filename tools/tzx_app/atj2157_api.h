#ifndef ATJ2157_API_H
#define ATJ2157_API_H

#include <stdint.h>
#include <stddef.h>

/* =========================================================================
 * ATJ2157 (ARMv7-M / Cortex-M Thumb-2) Verified Kernel & Driver Syscall ABI
 * Each `svc #imm8` returns directly to caller `LR` via the kernel SVCall
 * exception handler (stacked PC <- stacked LR & ~1).
 * ========================================================================= */

void *sys_op_entry(void *p1, void *p2, void *p3, uint32_t cmd);
void *libc_op_entry(void *p1, void *p2, void *p3, uint32_t cmd);
void *audio_device_op_entry(void *p1, void *p2, void *p3, uint32_t cmd);
void *lcd_op_entry(void *p1, void *p2, void *p3, uint32_t cmd);
void *key_op_entry(void *p1, void *p2, void *p3, uint32_t cmd);
void *vfs_op_entry(void *mnt, void *p1, void *p2, void *p3, uint32_t cmd);

/* --- KERNEL.DRV (svc #0x0f) verified commands --- */
#define KERN_DETECT_DISK        0x07u
#define KERN_DRV_INSTALL        0x08u
#define KERN_DRV_UNINSTALL      0x09u
#define KERN_GET_DRV_INFO       0x0Au
#define KERN_VM_READ            0x12u
#define KERN_VM_WRITE           0x13u
#define KERN_MQ_SEND            0x18u
#define KERN_MQ_RECEIVE         0x19u
#define KERN_MQ_FLUSH           0x1Au
#define KERN_GET_AB_TIMER       0x1Cu
#define KERN_MDELAY             0x1Du
#define KERN_UDELAY             0x1Eu
#define KERN_MOUNT_FS           0x29u
#define KERN_UNMOUNT_FS         0x2Au
#define KERN_OS_TIME_DLY        0x31u
#define KERN_OS_SCHED_LOCK      0x33u
#define KERN_OS_SCHED_UNLOCK    0x34u
#define KERN_ADJUST_CLK         0x3Eu

/* --- LIBC (svc #0x10) verified commands --- */
#define LIBC_MEMCPY             0x00u
#define LIBC_MEMSET             0x01u
#define LIBC_PTHREAD_CREATE     0x0Du
#define LIBC_EXIT               0x0Eu
#define LIBC_GET_PROC_STRUCT    0x10u
#define LIBC_FREE_PROC_STRUCT   0x11u

/* --- AUD_DEV.DRV (svc #0x0b) verified ATJ2157 commands --- */
#define AUD_ENABLE_PA           0x00u
#define AUD_DISABLE_PA          0x01u
#define AUD_ENABLE_PA_DDV       0x02u
#define AUD_DISABLE_PA_DDV      0x03u
#define AUD_SET_PA_VOL          0x04u
#define AUD_PCM_INIT            0x07u
#define AUD_PCM_CLOSE           0x08u
#define AUD_SET_DAC_VOL         0x0Bu
#define AUD_PCM_WRITE           0x0Du
#define AUD_PCM_QUERY           0x0Eu

/* --- DRV_LCD.DRV (svc #0x07) verified ATJ2157 commands --- */
#define LCD_CMD_SET_WINDOW      0x00u
#define LCD_CMD_SET_CONTRAST    0x01u
#define LCD_CMD_SET_DRAWMODE    0x02u
#define LCD_CMD_STANDBY         0x03u
#define LCD_CMD_BACKLIGHT       0x04u
#define LCD_CMD_TRANS_BUFFDATA  0x06u

/* --- VFS (svc #0x11) verified commands --- */
#define VFS_CMD_CD              0x00u
#define VFS_CMD_DIR             0x01u
#define VFS_CMD_GET_NAME        0x02u
#define VFS_CMD_FILE_DIR_OFFSET 0x03u
#define VFS_CMD_FILE_OPEN       0x05u
#define VFS_CMD_FILE_CLOSE      0x07u
#define VFS_CMD_FILE_GET_SIZE   0x09u
#define VFS_CMD_FILE_SEEK       0x0Bu
#define VFS_CMD_FILE_TELL       0x0Cu
#define VFS_CMD_FILE_READ       0x0Du

#define VFS_CD_UP               '\\'
#define VFS_CD_BACK             '|'
#define VFS_CD_ROOT             ':'
#define VFS_CD_SUB              '/'

#define VFS_DIR_NEXT            0x80u
#define VFS_DIR_PREV            0x81u
#define VFS_DIR_HEAD            0x82u
#define VFS_DIR_TAIL            0x83u

#define VFS_EXT_ALL_FILE        0x00000002u
#define VFS_EXT_ALL_DIR         0x00000001u
#define VFS_R_NORMAL_SEEK       0x00u
#define VFS_SEEK_SET            0u

#define DRV_GROUP_STG_BASE      0u
#define DRV_GROUP_STG_CARD      1u
#define DISK_C                  7u
#define DISK_H                  0x10u

/* --- Message Queues & App IDs --- */
#define MQ_ID_MNG               0u
#define MQ_ID_DESK              1u
#define MQ_ID_EGN               2u
#define MQ_ID_SYS               3u
#define MQ_ID_GUI               4u

#define APP_ID_TOOLS            0x09u
#define APP_ID_MANAGER          0x0Eu
#define APP_ID_MAINMENU         0x0Fu
#define APP_ID_MENGINE          0x13u
#define APP_ID_FMENGINE         0x14u
#define PARAM_FROM_TOOLS        0x0Au

#define MSG_POWER_OFF           0x0084u
#define MSG_USB_STICK           0x0090u
#define MSG_SD_IN               0x00B0u
#define MSG_SD_OUT              0x00B1u
#define MSG_APP_QUIT            0x0102u
#define MSG_CREAT_APP           0x0201u
#define MSG_KILL_APP_SYNC       0x0204u

/* --- Physical Key Values (verified against CFG_KEY_ARRAY & gui_msg.h) --- */
#define KEY_MODE                0x03u   /* Right side: M */
#define KEY_PREV                0x04u   /* Right side: RWD (<<) */
#define KEY_PLAY                0x05u   /* Front: Play/Pause */
#define KEY_NEXT                0x06u   /* Right side: FWD (>>) */
#define KEY_VSUB                0x07u   /* Left side: - (Vol-) */
#define KEY_REC                 0x08u   /* Front: Record */
#define KEY_VADD                0x09u   /* Left side: + (Vol+) */
#define KEY_VOL                 0x0Au
#define KEY_RETURN              0x80u   /* Front: Stop */
#define KEY_POWER               0x81u   /* Left side: Power */

#define KEY_TYPE_DOWN           0x2000u
#define KEY_TYPE_LONG           0x1000u
#define KEY_TYPE_HOLD           0x0800u
#define KEY_TYPE_SHORT_UP       0x0400u
#define KEY_TYPE_LONG_UP        0x0200u
#define KEY_TYPE_HOLD_UP        0x0100u

/* --- Verified ATJ2157 APP_GLOBE_DATA Shared OS Structures --- */
typedef struct {
    uint8_t  used;
    uint8_t  app_id;
    uint8_t  app_type;
    uint8_t  mq_id;
    uint8_t  screensaver_forbidden;
    uint8_t  auto_standby_forbidden;
    uint8_t  timed_shutdown_forbidden;
    uint8_t  soft_watchdog_forbidden;
    uint32_t soft_watchdog_timeout_expires;
} app_info_t; /* 12 bytes @ 0x00202f90 */

typedef struct {
    uint8_t  type;
    uint8_t  state;
    uint8_t  app_state;
    uint8_t  filter_key;
    uint8_t  keylock_state;
    uint8_t  sound_state;
    uint8_t  volume;
    uint8_t  backlight_state;
    uint8_t  screensave_state;
    uint8_t  card_state;
    uint8_t  cable_state;
    uint8_t  antenna_state;
    uint8_t  charge_state;
    uint8_t  bat_value;
    uint8_t  card_plist;
    uint8_t  card_update;
    uint8_t  video_state;
} app_info_state_t; /* @ 0x00202fc0 */

#define G_APP_INFO_VECTOR   ((volatile app_info_t *)0x00202F90u)
#define G_APP_INFO_STATE    ((volatile app_info_state_t *)0x00202FC0u)

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
} lcd_region_t;

typedef struct {
    uint32_t type;
    uint8_t  data[4];
} msg_apps_t;

typedef struct {
    msg_apps_t msg;
    void      *sem;
    void      *reply;
} private_msg_t;

#endif /* ATJ2157_API_H */
