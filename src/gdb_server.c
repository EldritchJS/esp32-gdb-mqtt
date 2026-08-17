#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "gdb_server.h"
#include "mqtt_transport.h"
#include "target.h"
#include "target_local.h"
#include "target_jtag.h"
#ifdef CONFIG_JTAG_TRANSPORT_ECP5
#include "ecp5_jtag.h"
#endif
#include "ramfs.h"
#include "riscv_dm.h"
#include "esp_http_client.h"

static const char *TAG = "gdb-server";

#define GDB_BUF_SIZE 1024
#define GDB_TASK_STACK 8192
#define MAX_THREADS 32

static uint8_t s_pkt_buf[GDB_BUF_SIZE];
static uint8_t s_resp_buf[GDB_BUF_SIZE * 2];

static int s_current_tid;
static target_thread_info_t s_thread_list[MAX_THREADS];
static int s_thread_count;
static bool s_running;

static const char hex_chars[] = "0123456789abcdef";
static bool s_pending_ack;

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t hex_encode(const uint8_t *data, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex_chars[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex_chars[data[i] & 0xf];
    }
    return len * 2;
}

static size_t hex_decode(const char *hex, size_t hex_len, uint8_t *out)
{
    size_t n = hex_len / 2;
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return i;
        out[i] = (hi << 4) | lo;
    }
    return n;
}

static uint32_t parse_hex(const char *s, const char **end)
{
    uint32_t val = 0;
    while (*s) {
        int h = hex_val(*s);
        if (h < 0) break;
        val = (val << 4) | h;
        s++;
    }
    if (end) *end = s;
    return val;
}

static void send_rsp_packet(const char *payload, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum += (uint8_t)payload[i];
    }

    size_t pos = 0;
    if (s_pending_ack) {
        s_resp_buf[pos++] = '+';
        s_pending_ack = false;
    }
    s_resp_buf[pos++] = '$';
    memcpy(&s_resp_buf[pos], payload, len);
    pos += len;
    s_resp_buf[pos++] = '#';
    s_resp_buf[pos++] = hex_chars[(checksum >> 4) & 0xf];
    s_resp_buf[pos++] = hex_chars[checksum & 0xf];

    ESP_LOGI(TAG, "RSP> %.*s (%d bytes)", (int)(len > 40 ? 40 : len), payload, (int)pos);
    mqtt_transport_send(s_resp_buf, pos);
}

static void send_rsp_str(const char *str)
{
    send_rsp_packet(str, strlen(str));
}

static void send_empty(void)
{
    send_rsp_str("");
}

static void send_ok(void)
{
    send_rsp_str("OK");
}

static void refresh_thread_list(void)
{
    s_thread_count = target_get_threads(s_thread_list, MAX_THREADS);
    if (s_thread_count > 0 && s_current_tid == 0) {
        s_current_tid = s_thread_list[0].id;
    }
}

static const char s_target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>riscv:rv32</architecture>"
    "<feature name=\"org.gnu.gdb.riscv.cpu\">"
    "<reg name=\"zero\" bitsize=\"32\" regnum=\"0\" type=\"uint32\"/>"
    "<reg name=\"ra\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"gp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"tp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"t0\" bitsize=\"32\"/>"
    "<reg name=\"t1\" bitsize=\"32\"/>"
    "<reg name=\"t2\" bitsize=\"32\"/>"
    "<reg name=\"fp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"s1\" bitsize=\"32\"/>"
    "<reg name=\"a0\" bitsize=\"32\"/>"
    "<reg name=\"a1\" bitsize=\"32\"/>"
    "<reg name=\"a2\" bitsize=\"32\"/>"
    "<reg name=\"a3\" bitsize=\"32\"/>"
    "<reg name=\"a4\" bitsize=\"32\"/>"
    "<reg name=\"a5\" bitsize=\"32\"/>"
    "<reg name=\"a6\" bitsize=\"32\"/>"
    "<reg name=\"a7\" bitsize=\"32\"/>"
    "<reg name=\"s2\" bitsize=\"32\"/>"
    "<reg name=\"s3\" bitsize=\"32\"/>"
    "<reg name=\"s4\" bitsize=\"32\"/>"
    "<reg name=\"s5\" bitsize=\"32\"/>"
    "<reg name=\"s6\" bitsize=\"32\"/>"
    "<reg name=\"s7\" bitsize=\"32\"/>"
    "<reg name=\"s8\" bitsize=\"32\"/>"
    "<reg name=\"s9\" bitsize=\"32\"/>"
    "<reg name=\"s10\" bitsize=\"32\"/>"
    "<reg name=\"s11\" bitsize=\"32\"/>"
    "<reg name=\"t3\" bitsize=\"32\"/>"
    "<reg name=\"t4\" bitsize=\"32\"/>"
    "<reg name=\"t5\" bitsize=\"32\"/>"
    "<reg name=\"t6\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "</feature>"
    "</target>";

static void handle_q_supported(void)
{
    send_rsp_str("PacketSize=400;qXfer:features:read+");
}

static void handle_xfer_features(const char *pkt)
{
    if (strncmp(pkt, "qXfer:features:read:target.xml:", 31) != 0) {
        send_rsp_str("E00");
        return;
    }

    const char *p = pkt + 31;
    uint32_t offset = parse_hex(p, &p);
    if (*p == ',') p++;
    uint32_t length = parse_hex(p, NULL);

    size_t xml_len = strlen(s_target_xml);
    if (offset >= xml_len) {
        send_rsp_str("l");
        return;
    }

    size_t avail = xml_len - offset;
    bool last = (avail <= length);
    if (avail > length) avail = length;

    char buf[1024];
    buf[0] = last ? 'l' : 'm';
    memcpy(buf + 1, s_target_xml + offset, avail);
    send_rsp_packet(buf, avail + 1);
}

static void handle_halt_reason(void)
{
    if (s_running) {
        target_halt();
        s_running = false;
        refresh_thread_list();
    }
    send_rsp_str("S05");
}

static void handle_read_registers(void)
{
    uint32_t regs[TARGET_NUM_REGS];

    if (s_current_tid == 0 || target_read_registers(s_current_tid, regs) != 0) {
        send_rsp_str("E01");
        return;
    }

    char resp[TARGET_NUM_REGS * 8 + 1];
    for (int i = 0; i < TARGET_NUM_REGS; i++) {
        uint8_t bytes[4];
        bytes[0] = (regs[i]) & 0xff;
        bytes[1] = (regs[i] >> 8) & 0xff;
        bytes[2] = (regs[i] >> 16) & 0xff;
        bytes[3] = (regs[i] >> 24) & 0xff;
        hex_encode(bytes, 4, &resp[i * 8]);
    }
    resp[TARGET_NUM_REGS * 8] = '\0';
    send_rsp_str(resp);
}

static void handle_read_register(const char *pkt)
{
    const char *p = pkt + 1;
    uint32_t reg_num = parse_hex(p, NULL);

    uint32_t regs[TARGET_NUM_REGS];
    if (s_current_tid == 0 || target_read_registers(s_current_tid, regs) != 0) {
        send_rsp_str("E01");
        return;
    }

    if (reg_num >= TARGET_NUM_REGS) {
        send_rsp_str("E01");
        return;
    }

    char resp[9];
    uint8_t bytes[4];
    bytes[0] = (regs[reg_num]) & 0xff;
    bytes[1] = (regs[reg_num] >> 8) & 0xff;
    bytes[2] = (regs[reg_num] >> 16) & 0xff;
    bytes[3] = (regs[reg_num] >> 24) & 0xff;
    hex_encode(bytes, 4, resp);
    resp[8] = '\0';
    send_rsp_str(resp);
}

static void handle_write_register(const char *pkt)
{
    const char *p = pkt + 1;
    uint32_t reg_num = parse_hex(p, &p);
    if (*p == '=') p++;

    uint8_t bytes[4];
    hex_decode(p, 8, bytes);
    uint32_t value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);

    if (s_current_tid == 0 ||
        target_write_register(s_current_tid, reg_num, value) != 0) {
        send_rsp_str("E01");
        return;
    }
    send_ok();
}

static void handle_read_memory(const char *pkt)
{
    const char *p = pkt + 1;
    uint32_t addr = parse_hex(p, &p);
    if (*p == ',') p++;
    uint32_t len = parse_hex(p, NULL);

    if (len > GDB_BUF_SIZE / 2) {
        len = GDB_BUF_SIZE / 2;
    }

    uint8_t mem[GDB_BUF_SIZE / 2];
    if (target_read_memory(addr, mem, len) != 0) {
        send_rsp_str("E01");
        return;
    }

    char resp[GDB_BUF_SIZE + 1];
    size_t hex_len = hex_encode(mem, len, resp);
    resp[hex_len] = '\0';
    send_rsp_str(resp);
}

static void handle_write_memory(const char *pkt)
{
    const char *p = pkt + 1;
    uint32_t addr = parse_hex(p, &p);
    if (*p == ',') p++;
    uint32_t len = parse_hex(p, &p);
    if (*p == ':') p++;

    if (len > GDB_BUF_SIZE / 2) {
        send_rsp_str("E01");
        return;
    }

    uint8_t data[GDB_BUF_SIZE / 2];
    hex_decode(p, len * 2, data);

    if (target_write_memory(addr, data, len) != 0) {
        send_rsp_str("E01");
        return;
    }
    send_ok();
}

static void handle_breakpoint_set(const char *pkt)
{
    const char *p = pkt + 2;
    if (*p == ',') p++;
    uint32_t addr = parse_hex(p, NULL);

    if (target_set_breakpoint(addr) == 0) {
        send_ok();
    } else {
        send_rsp_str("E01");
    }
}

static void handle_breakpoint_clear(const char *pkt)
{
    const char *p = pkt + 2;
    if (*p == ',') p++;
    uint32_t addr = parse_hex(p, NULL);

    if (target_clear_breakpoint(addr) == 0) {
        send_ok();
    } else {
        send_rsp_str("E01");
    }
}

static void handle_continue(void)
{
    target_continue();
    s_running = true;
}

static void handle_set_thread(const char *pkt)
{
    const char *p = pkt + 2;
    int tid;

    if (*p == '-' && *(p + 1) == '1') {
        tid = 1;
    } else if (*p == '0') {
        tid = 1;
    } else {
        tid = (int)parse_hex(p, NULL);
    }

    if (tid >= 1 && tid <= s_thread_count) {
        s_current_tid = tid;
    }
    send_ok();
}

static void handle_thread_alive(const char *pkt)
{
    const char *p = pkt + 1;
    int tid = (int)parse_hex(p, NULL);

    if (tid >= 1 && tid <= s_thread_count) {
        send_ok();
    } else {
        send_rsp_str("E01");
    }
}

static void handle_thread_info_first(void)
{
    refresh_thread_list();

    if (s_thread_count == 0) {
        send_rsp_str("l");
        return;
    }

    char resp[256] = "m";
    size_t pos = 1;
    for (int i = 0; i < s_thread_count && pos < sizeof(resp) - 8; i++) {
        if (i > 0) {
            resp[pos++] = ',';
        }
        pos += snprintf(&resp[pos], sizeof(resp) - pos, "%x", s_thread_list[i].id);
    }
    send_rsp_str(resp);
}

static void handle_thread_info_next(void)
{
    send_rsp_str("l");
}

static void handle_thread_extra_info(const char *pkt)
{
    const char *p = pkt + strlen("qThreadExtraInfo,");
    int tid = (int)parse_hex(p, NULL);

    target_thread_info_t *info = NULL;
    for (int i = 0; i < s_thread_count; i++) {
        if (s_thread_list[i].id == tid) {
            info = &s_thread_list[i];
            break;
        }
    }

    if (!info) {
        send_rsp_str("E01");
        return;
    }

    const char *state_str;
    switch (info->state) {
        case TARGET_STATE_RUNNING: state_str = "Running"; break;
        case TARGET_STATE_HALTED:  state_str = "Halted"; break;
        case TARGET_STATE_BLOCKED: state_str = "Blocked"; break;
        default:                   state_str = "Unknown"; break;
    }

    char desc[64];
    snprintf(desc, sizeof(desc), "%s (pri=%lu, %s)",
             info->name, (unsigned long)info->priority, state_str);

    char resp[128];
    size_t hex_len = hex_encode((const uint8_t *)desc, strlen(desc), resp);
    resp[hex_len] = '\0';
    send_rsp_str(resp);
}

static void handle_current_thread(void)
{
    char resp[16];
    snprintf(resp, sizeof(resp), "QC%x", s_current_tid);
    send_rsp_str(resp);
}

static void handle_monitor_cmd(const char *pkt)
{
    const char *p = pkt + strlen("qRcmd,");
    char cmd[64];
    size_t cmd_len = hex_decode(p, strlen(p), (uint8_t *)cmd);
    cmd[cmd_len] = '\0';

    if (strcmp(cmd, "target local") == 0) {
        target_set_backend(&target_local_ops);
        target_local_set_excluded_task(xTaskGetCurrentTaskHandle());
        refresh_thread_list();
        target_halt();
        s_running = false;

        const char *msg = "Switched to local backend\n";
        char resp[128];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
    } else if (strcmp(cmd, "target jtag") == 0) {
        target_set_backend(&target_jtag_ops);
        refresh_thread_list();
        target_halt();
        s_running = false;

        const char *msg = "Switched to JTAG backend\n";
        char resp[128];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
    } else if (strcmp(cmd, "target") == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Current backend: %s\n", target_backend_name());
        char resp[128];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
#ifdef CONFIG_JTAG_TRANSPORT_ECP5
    } else if (strcmp(cmd, "fpga_idcode") == 0) {
        uint32_t id = ecp5_read_idcode();
        char msg[64];
        snprintf(msg, sizeof(msg), "ECP5 IDCODE: 0x%08lx\n", (unsigned long)id);
        char resp[128];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
    } else if (strcmp(cmd, "fpga_status") == 0) {
        uint32_t st = ecp5_read_status();
        char msg[96];
        snprintf(msg, sizeof(msg), "ECP5 status: 0x%08lx (DONE=%d BUSY=%d FAIL=%d)\n",
                 (unsigned long)st,
                 (st & ECP5_STATUS_DONE) ? 1 : 0,
                 (st & ECP5_STATUS_BUSY) ? 1 : 0,
                 (st & ECP5_STATUS_FAIL) ? 1 : 0);
        char resp[192];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
#endif
    } else if (strcmp(cmd, "files") == 0) {
        const ramfs_file_t *entries[RAMFS_MAX_FILES];
        int count = ramfs_list(entries, RAMFS_MAX_FILES);
        char msg[512];
        int pos = 0;
        if (count == 0) {
            pos = snprintf(msg, sizeof(msg), "No files stored\n");
        } else {
            for (int i = 0; i < count && pos < (int)sizeof(msg) - 64; i++) {
                pos += snprintf(msg + pos, sizeof(msg) - pos, "  %s (%zu bytes)\n",
                                entries[i]->name, entries[i]->size);
            }
        }
        char resp[1024];
        size_t len = hex_encode((const uint8_t *)msg, pos, resp);
        resp[len] = '\0';
        send_rsp_str(resp);
#ifdef CONFIG_JTAG_TRANSPORT_ECP5
    } else if (strncmp(cmd, "fpga_stream ", 12) == 0) {
        const char *url = cmd + 12;
        esp_http_client_config_t http_cfg = {
            .url = url,
            .timeout_ms = 30000,
        };
        esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
        if (!http) {
            const char *msg = "HTTP client init failed\n";
            char resp[128];
            size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
            resp[len] = '\0';
            send_rsp_str(resp);
        } else {
            esp_err_t err = esp_http_client_open(http, 0);
            int status_code = 0;
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(http);
                status_code = esp_http_client_get_status_code(http);
            }
            if (err != ESP_OK || status_code != 200) {
                char msg[64];
                snprintf(msg, sizeof(msg), "HTTP failed (status %d)\n", status_code);
                char resp[128];
                size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
                resp[len] = '\0';
                send_rsp_str(resp);
                esp_http_client_close(http);
                esp_http_client_cleanup(http);
            } else {
                int rc = ecp5_program_begin();
                if (rc != 0) {
                    const char *msg = "FPGA program_begin failed\n";
                    char resp[128];
                    size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
                    resp[len] = '\0';
                    send_rsp_str(resp);
                    esp_http_client_close(http);
                    esp_http_client_cleanup(http);
                } else {
                    uint8_t chunk[512];
                    size_t total = 0;
                    bool ok = true;
                    while (1) {
                        int rd = esp_http_client_read(http, (char *)chunk, sizeof(chunk));
                        if (rd < 0) {
                            ok = false;
                            break;
                        }
                        if (rd == 0)
                            break;
                        ecp5_program_data(chunk, rd);
                        total += rd;
                    }
                    esp_http_client_close(http);
                    esp_http_client_cleanup(http);

                    char msg[96];
                    if (!ok) {
                        ecp5_program_end();
                        snprintf(msg, sizeof(msg), "HTTP read error after %zu bytes\n", total);
                    } else {
                        rc = ecp5_program_end();
                        if (rc == 0) {
                            snprintf(msg, sizeof(msg),
                                     "Streamed and programmed %zu bytes OK\n", total);
                        } else {
                            snprintf(msg, sizeof(msg),
                                     "Programming failed after streaming %zu bytes\n", total);
                        }
                    }
                    char resp[192];
                    size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
                    resp[len] = '\0';
                    send_rsp_str(resp);
                }
            }
        }
    } else if (strncmp(cmd, "fpga_program ", 13) == 0) {
        const char *name = cmd + 13;
        const ramfs_file_t *f = ramfs_find(name);
        if (!f) {
            const char *msg = "File not found\n";
            char resp[64];
            size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
            resp[len] = '\0';
            send_rsp_str(resp);
        } else {
            int rc = ecp5_program_bitstream(f->data, f->size);
            char msg[96];
            if (rc == 0) {
                snprintf(msg, sizeof(msg), "Programmed FPGA with %s (%zu bytes)\n",
                         name, f->size);
            } else {
                snprintf(msg, sizeof(msg), "FPGA programming failed (rc=%d)\n", rc);
            }
            char resp[192];
            size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
            resp[len] = '\0';
            send_rsp_str(resp);
        }
#endif
    } else if (strncmp(cmd, "riscv_load ", 11) == 0) {
        const char *args = cmd + 11;
        char name[RAMFS_MAX_FILENAME];
        uint32_t addr = 0x10000000;

        const char *space = strchr(args, ' ');
        if (space) {
            size_t nlen = space - args;
            if (nlen >= RAMFS_MAX_FILENAME) nlen = RAMFS_MAX_FILENAME - 1;
            memcpy(name, args, nlen);
            name[nlen] = '\0';
            addr = (uint32_t)strtoul(space + 1, NULL, 0);
        } else {
            strlcpy(name, args, sizeof(name));
        }

        const ramfs_file_t *f = ramfs_find(name);
        if (!f) {
            const char *msg = "File not found\n";
            char resp[64];
            size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
            resp[len] = '\0';
            send_rsp_str(resp);
        } else {
            dm_halt();
            int rc = dm_write_memory(addr, f->data, f->size);
            if (rc != 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Memory write failed (rc=%d)\n", rc);
                char resp[128];
                size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
                resp[len] = '\0';
                send_rsp_str(resp);
            } else {
                dm_write_csr(AC_REG_DPC, addr);
                dm_resume();
                char msg[96];
                snprintf(msg, sizeof(msg), "Loaded %s (%zu bytes) at 0x%08lx, running\n",
                         name, f->size, (unsigned long)addr);
                char resp[192];
                size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
                resp[len] = '\0';
                send_rsp_str(resp);
            }
        }
    } else {
        const char *msg = "Commands:\n"
                          "  target [local|jtag]\n"
                          "  fpga_idcode\n"
                          "  fpga_status\n"
                          "  fpga_stream <url>\n"
                          "  fpga_program <filename>\n"
                          "  riscv_load <filename> [address]\n"
                          "  files\n";
        char resp[512];
        size_t len = hex_encode((const uint8_t *)msg, strlen(msg), resp);
        resp[len] = '\0';
        send_rsp_str(resp);
    }
}

static void handle_query(const char *pkt)
{
    if (strncmp(pkt, "qSupported", 10) == 0) {
        handle_q_supported();
    } else if (strcmp(pkt, "qfThreadInfo") == 0) {
        handle_thread_info_first();
    } else if (strcmp(pkt, "qsThreadInfo") == 0) {
        handle_thread_info_next();
    } else if (strncmp(pkt, "qThreadExtraInfo,", 17) == 0) {
        handle_thread_extra_info(pkt);
    } else if (strcmp(pkt, "qC") == 0) {
        handle_current_thread();
    } else if (strcmp(pkt, "qAttached") == 0) {
        send_rsp_str("1");
    } else if (strncmp(pkt, "qRcmd,", 6) == 0) {
        handle_monitor_cmd(pkt);
    } else if (strncmp(pkt, "qXfer:features:read:", 20) == 0) {
        handle_xfer_features(pkt);
    } else if (strncmp(pkt, "qTStatus", 8) == 0) {
        send_rsp_str("T0");
    } else {
        send_empty();
    }
}

static void handle_packet(const char *pkt, size_t len)
{
    if (len == 0) return;

    switch (pkt[0]) {
    case '?':
        handle_halt_reason();
        break;
    case 'g':
        handle_read_registers();
        break;
    case 'G':
        send_empty();
        break;
    case 'p':
        handle_read_register(pkt);
        break;
    case 'P':
        handle_write_register(pkt);
        break;
    case 'm':
        handle_read_memory(pkt);
        break;
    case 'M':
        handle_write_memory(pkt);
        break;
    case 'c':
        handle_continue();
        break;
    case 's':
        send_empty();
        break;
    case 'H':
        handle_set_thread(pkt);
        break;
    case 'T':
        handle_thread_alive(pkt);
        break;
    case 'Z':
        if (pkt[1] == '0' || pkt[1] == '1') {
            handle_breakpoint_set(pkt);
        } else {
            send_empty();
        }
        break;
    case 'z':
        if (pkt[1] == '0' || pkt[1] == '1') {
            handle_breakpoint_clear(pkt);
        } else {
            send_empty();
        }
        break;
    case 'k':
        target_clear_all_breakpoints();
        target_continue();
        s_running = false;
        send_ok();
        break;
    case 'D':
        target_clear_all_breakpoints();
        target_continue();
        s_running = false;
        send_ok();
        break;
    case 'q':
        handle_query(pkt);
        break;
    case 'v':
        if (strncmp(pkt, "vMustReplyEmpty", 15) == 0) {
            send_empty();
        } else {
            send_empty();
        }
        break;
    default:
        send_empty();
        break;
    }
}

static void gdb_server_task(void *arg)
{
    uint8_t rx_buf[256];
    enum { WAIT_START, IN_PACKET, IN_CHECKSUM } state = WAIT_START;
    size_t pkt_len = 0;
    uint8_t cksum_buf[2];
    int cksum_pos = 0;

    ESP_LOGI(TAG, "GDB server task started (backend: %s)", target_backend_name());

    target_local_set_excluded_task(xTaskGetCurrentTaskHandle());
    refresh_thread_list();
    s_running = true;

    while (1) {
        if (s_running) {
            int hit_tid;
            uint32_t hit_addr;
            if (target_check_breakpoints(&hit_tid, &hit_addr) > 0) {
                target_halt();
                s_running = false;
                s_current_tid = hit_tid;
                refresh_thread_list();
                ESP_LOGI(TAG, "Breakpoint hit at 0x%08lx", (unsigned long)hit_addr);
                send_rsp_str("S05");
            }
        }

        int n = mqtt_transport_recv(rx_buf, sizeof(rx_buf), 50);
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; i++) {
            uint8_t c = rx_buf[i];

            if (c == 0x03) {
                target_halt();
                s_running = false;
                refresh_thread_list();
                send_rsp_str("S05");
                state = WAIT_START;
                pkt_len = 0;
                continue;
            }

            switch (state) {
            case WAIT_START:
                if (c == '$') {
                    pkt_len = 0;
                    state = IN_PACKET;
                } else if (c == '+' || c == '-') {
                    /* ack/nack — ignore */
                }
                break;

            case IN_PACKET:
                if (c == '#') {
                    state = IN_CHECKSUM;
                    cksum_pos = 0;
                } else if (pkt_len < GDB_BUF_SIZE - 1) {
                    s_pkt_buf[pkt_len++] = c;
                }
                break;

            case IN_CHECKSUM:
                cksum_buf[cksum_pos++] = c;
                if (cksum_pos == 2) {
                    s_pkt_buf[pkt_len] = '\0';

                    uint8_t expected = (hex_val(cksum_buf[0]) << 4) |
                                        hex_val(cksum_buf[1]);
                    uint8_t actual = 0;
                    for (size_t j = 0; j < pkt_len; j++) {
                        actual += s_pkt_buf[j];
                    }

                    if (actual == expected) {
                        s_pending_ack = true;
                        ESP_LOGI(TAG, "PKT< %.*s", (int)(pkt_len > 40 ? 40 : pkt_len), s_pkt_buf);
                        handle_packet((const char *)s_pkt_buf, pkt_len);
                        if (s_pending_ack) {
                            uint8_t ack = '+';
                            mqtt_transport_send(&ack, 1);
                            s_pending_ack = false;
                        }
                    } else {
                        ESP_LOGW(TAG, "Bad checksum: got %02x want %02x", actual, expected);
                        uint8_t nack = '-';
                        mqtt_transport_send(&nack, 1);
                    }

                    state = WAIT_START;
                    pkt_len = 0;
                }
                break;
            }
        }
    }
}

esp_err_t gdb_server_init(void)
{
    target_init(&target_local_ops);
    target_local_set_excluded_task(xTaskGetCurrentTaskHandle());

    BaseType_t ret = xTaskCreate(gdb_server_task, "gdb_srv",
                                  GDB_TASK_STACK, NULL,
                                  tskIDLE_PRIORITY + 3, NULL);
    if (ret != pdPASS) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GDB server initialized");
    return ESP_OK;
}
