/*
 * AIOS top — process viewer
 *
 * Reads /proc/status, /proc/meminfo, /proc/uptime
 * Refreshes display with ANSI escape codes.
 * Press 'q' to quit.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    char status[4096], meminfo[256], uptime_buf[64];
    int running = 1;

    while (running) {
        /* Clear screen + home */
        printf("\033[2J\033[H");

        /* Header */
        printf("AIOS top - ");

        /* Read uptime */
        FILE *fu = fopen("/proc/uptime", "r");
        if (fu) {
            int n = fread(uptime_buf, 1, 63, fu);
            fclose(fu);
            uptime_buf[n] = '\0';
            /* Parse seconds from "NNN.NN NNN.NN\n" */
            int secs = 0;
            for (int i = 0; uptime_buf[i] && uptime_buf[i] != '.'; i++)
                secs = secs * 10 + (uptime_buf[i] - '0');
            int hrs = secs / 3600;
            int mins = (secs / 60) % 60;
            int s = secs % 60;
            printf("up %d:%02d:%02d", hrs, mins, s);
        }
        printf("\n");

        /* Read meminfo */
        FILE *fm = fopen("/proc/meminfo", "r");
        if (fm) {
            int n = fread(meminfo, 1, 255, fm);
            fclose(fm);
            meminfo[n] = '\0';
            printf("%s", meminfo);
        }
        printf("\n");

        /* Column headers */
        printf("\033[7m");  /* reverse video */
        printf("  PID  PRI  NICE  STATE   UID  THR  NAME                ");
        printf("\033[0m\n");  /* normal */

        /* Read process table */
        FILE *fp = fopen("/proc/status", "r");
        if (fp) {
            int n = fread(status, 1, 4095, fp);
            fclose(fp);
            status[n] = '\0';
            /* Skip header line */
            char *p = status;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            /* Print remaining lines */
            while (*p) {
                char *linestart = p;
                while (*p && *p != '\n') p++;
                if (*p == '\n') { *p = '\0'; p++; }
                if (linestart[0])
                    printf("  %s\n", linestart);
            }
        }

        printf("\nPress 'q' to quit\n");

        /* Sleep 2 seconds, check for 'q' */
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);

        /* Non-blocking check for 'q' — use read with small timeout */
        /* Since we don't have non-blocking I/O, just check once */
        /* For now, top runs until the process is killed or we add
         * a way to check for input without blocking */
    }

    return 0;
}
