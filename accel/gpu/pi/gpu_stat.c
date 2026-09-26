/*
 * gpu_stat -- FPGA-GPU status: daemon HELLO + all PL registers (NET_STATUS, as an observer, so it
 * never disturbs the running application) and optionally the Teensy's T_HELLO / T_STATS.
 *
 *   gpu_stat [--fpga HOST[:PORT]] [--teensy auto|DEV|tcp:H:P|none] [--watch [MS]] [--count N]
 *
 * --teensy defaults to none here: opening the Teensy's tty would take it away from (and reset the
 * message parser under) an application that is using it. --watch refreshes every MS (1000) and
 * shows rates (frames/s, vsync Hz, records/s, Teensy words/s).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tool_common.h"

#define CORE_HZ 148750000.0

static const char *reg_name[NET_STATUS_NREGS] = {
    "ID", "VERSION", "CONTROL", "STATUS", "FRAME_COUNT", "FB0", "FB1", "FRONT", "CLEAR_COLOR",
    "PS_FIFO_FREE", "T_FIFO_LEVEL", "LIST_OVERFLOW", "BAD_RECORDS", "T_WORDS", "RENDER_CYCLES",
    "PRIM_COUNT", "VSYNC_COUNT", "AXI_ERRORS", "DROPPED", "RET_ADDR", "RET_CTRL", "RET_STATUS",
    "RET_FRAME", "LAST_FRAME_NO"
};

static void decode(uint32_t off, uint32_t v, char *out, size_t n)
{
    out[0] = 0;
    switch (off) {
    case GPU_R_ID:
        snprintf(out, n, "%s", v == GPU_ID_VALUE ? "'GPU1' ok" : "WRONG (expect 0x47505531)");
        break;
    case GPU_R_CONTROL:
        snprintf(out, n, "%s%s%s", (v & GPU_CTL_SRC_TEENSY) ? "SRC_TEENSY " : "", (v & GPU_CTL_SRC_PS) ? "SRC_PS " : "",
                 (v & GPU_CTL_SCANOUT_EN) ? "SCANOUT_EN" : "colour-bars");
        break;
    case GPU_R_STATUS:
        snprintf(out, n, "%s%s%s%s%s%s%s", (v & GPU_ST_MMCM_LOCKED) ? "MMCM_LOCKED " : "MMCM-NOT-LOCKED ",
                 (v & GPU_ST_RASTER_BUSY) ? "RASTER_BUSY " : "", (v & GPU_ST_HPD) ? "HPD " : "no-monitor ",
                 (v & GPU_ST_TEENSY_ACTIVE) ? "TEENSY_ACTIVE " : "", (v & GPU_ST_WAIT_TEENSY) ? "WAIT_TEENSY " : "",
                 (v & GPU_ST_WAIT_PS) ? "WAIT_PS " : "", (v & GPU_ST_SWAP_PENDING) ? "SWAP_PENDING" : "");
        break;
    case GPU_R_RENDER_CYCLES:
        snprintf(out, n, "%.3f ms at 148.75 MHz", v / CORE_HZ * 1e3);
        break;
    case GPU_R_CLEAR_COLOR:
        snprintf(out, n, "rgb(%u,%u,%u)", ((v >> 11) & 31u) << 3, ((v >> 5) & 63u) << 2, (v & 31u) << 3);
        break;
    case GPU_R_RET_CTRL:
        snprintf(out, n, "%s", (v & GPU_RET_ENABLE) ? "RET_ENABLE" : "");
        break;
    case GPU_R_RET_STATUS:
        snprintf(out, n, "%s%s", (v & GPU_RET_FULL) ? "RET_FULL " : "", (v & GPU_RET_CAPTURING) ? "RET_CAPTURING" : "");
        break;
    default:
        break;
    }
}

static void usage(void)
{
    fprintf(stderr, "usage: gpu_stat [options] [--watch [MS]] [--count N]\n%s", tool_common_help());
}

int main(int argc, char **argv)
{
    tool_opts o;
    gpu_conn *g;
    teensy_conn *t = NULL;
    long watch_ms = 0, count = 0, iter = 0;
    int i, tfail = 0, tty = isatty(1);
    net_status_reply prev;
    double tprev_s = 0;
    int have_prev = 0;

    tool_opts_init(&o, "none");
    for (i = 1; i < argc; i++) {
        int r = tool_common_opt(&o, argc, argv, &i);
        if (r < 0)
            return 2;
        if (r)
            continue;
        if (!strcmp(argv[i], "--watch")) {
            watch_ms = 1000;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (tool_parse_int(argv[++i], 50, 3600000, &watch_ms) < 0) {
                    usage();
                    return 2;
                }
            }
        } else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
            if (tool_parse_int(argv[++i], 1, 1000000000L, &count) < 0) {
                usage();
                return 2;
            }
        } else {
            usage();
            return strcmp(argv[i], "-h") && strcmp(argv[i], "--help") ? 2 : 0;
        }
    }
    tool_catch_signals();
    g = tool_connect(&o, 1, "gpu_stat");
    if (!g)
        return 1;
    t = tool_teensy(&o, 0, "gpu_stat", &tfail);
    if (tfail) {
        gpu_close(g);
        return 1;
    }

    for (;;) {
        net_status_reply s;
        t_stats_reply ts;
        t_hello_reply th;
        const net_hello_reply *h = gpu_hello_info(g);
        double now = fgpu_now();
        int st = gpu_status(g, &s), k, tst = -1;
        if (st <= -100) {
            fprintf(stderr, "gpu_stat: STATUS failed: %s\n", gpu_errmsg(g));
            break;
        }
        if (t) {
            tst = teensy_hello(t, &th);
            if (tst >= 0)
                tst = teensy_stats(t, &ts);
        }
        if (watch_ms && tty)
            printf("\033[H\033[J");
        printf("daemon %s:%d  protocol %u  PL id 0x%08x version 0x%08x  %ux%u  pool %u KB used of %u KB, "
               "max %u sprites\n", o.host, o.port, h->proto_version, h->pl_id, h->pl_version, h->width, h->height,
               s.pool_used / 1024u, h->pool_size / 1024u, h->max_sprites);
        if (st != GPU_ERR_OK) {
            printf("STATUS: %s\n", fgpu_strerror(st));
        } else {
            for (k = 0; k < NET_STATUS_NREGS; k++) {
                char d[160];
                decode((uint32_t)k * 4u, s.regs[k], d, sizeof d);
                printf("  %03x %-14s 0x%08x %10u  %s\n", k * 4, reg_name[k], s.regs[k], s.regs[k], d);
            }
            printf("  daemon: errors %u, records pushed %u\n", s.daemon_errors, s.daemon_records);
            if (have_prev) {
                double dt = now - tprev_s;
                printf("  rates over %.2f s: %.1f frames/s, vsync %.2f Hz, %.0f records/s (daemon), "
                       "%.0f Teensy words/s\n", dt,
                       (uint32_t)(gpu_reg(&s, GPU_R_FRAME_COUNT) - gpu_reg(&prev, GPU_R_FRAME_COUNT)) / dt,
                       (uint32_t)(gpu_reg(&s, GPU_R_VSYNC_COUNT) - gpu_reg(&prev, GPU_R_VSYNC_COUNT)) / dt,
                       (uint32_t)(s.daemon_records - prev.daemon_records) / dt,
                       (uint32_t)(gpu_reg(&s, GPU_R_T_WORDS) - gpu_reg(&prev, GPU_R_T_WORDS)) / dt);
            }
        }
        if (t) {
            if (tst < 0) {
                printf("teensy %s: %s %s\n", teensy_device(t), fgpu_strerror(tst), tst <= -100 ? teensy_errmsg(t) : "");
            } else {
                printf("teensy %s: fw %u  fpga_ready %u  bus_enabled %u  limits %u meshes %u verts %u indices\n",
                       teensy_device(t), th.fw_version, th.fpga_ready, th.bus_enabled, th.max_meshes, th.max_verts,
                       th.max_indices);
                printf("  frames %u  records %u  words %u  bus_timeouts %u  usb_bad_msgs %u  cpu %u MHz\n",
                       ts.frames, ts.records_sent, ts.words_sent, ts.bus_timeouts, ts.usb_bad_msgs, ts.cpu_mhz);
                printf("  autonomous: running %u  frames %u  %.2f fps  cpu busy %u%%  %u tris/s\n", ts.auto_running,
                       ts.auto_frames, ts.auto_fps_x100 / 100.0, ts.cpu_busy_pct, ts.tris_per_sec);
            }
        }
        fflush(stdout);
        prev = s;
        tprev_s = now;
        have_prev = st == GPU_ERR_OK;
        iter++;
        if (!watch_ms || tool_stop || (count && iter >= count))
            break;
        {
            long left = watch_ms;
            while (left > 0 && !tool_stop) {
                fgpu_sleep_ms(left > 100 ? 100 : (int)left);
                left -= 100;
            }
        }
        if (tool_stop)
            break;
    }
    teensy_close(t);
    gpu_close(g);
    return 0;
}
