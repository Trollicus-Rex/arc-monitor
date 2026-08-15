// arc_cli.c — command-line client for the multi-card arcd daemon.
// The daemon exposes one object per card at /org/freedesktop/ArcCtrl/cardN.
// This client enumerates card0, card1, ... (probing until one isn't there),
// shows a live block per card, and targets control methods at a chosen card.
//
// Build: gcc -O2 -Wall -Wextra -o arc-cli arc_cli.c -lsystemd
//
// Monitor:  ./arc-cli
// Control:  ./arc-cli [--card N] --set-pl1 <watts>
//           ./arc-cli [--card N] --lock-clock <mhz>
//           ./arc-cli [--card N] --reset-clock
//           ./arc-cli [--card N] --set-fan <percent>
//           ./arc-cli [--card N] --auto-fan
// (--card defaults to 0)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <systemd/sd-bus.h>

#define SVC   "org.freedesktop.ArcCtrl"
#define IFACE "org.freedesktop.ArcCtrl"
#define MAX_PROBE 64

static void card_path(int n, char *out, size_t sz) {
    snprintf(out, sz, "/org/freedesktop/ArcCtrl/card%d", n);
}

// Count cards by probing card0.. until GpuName can't be read.
static int count_cards(sd_bus *bus) {
    int n = 0;
    for (; n < MAX_PROBE; n++) {
        char path[64]; card_path(n, path, sizeof(path));
        sd_bus_error e = SD_BUS_ERROR_NULL;
        char *name = NULL;
        int r = sd_bus_get_property_string(bus, SVC, path, IFACE, "GpuName", &e, &name);
        sd_bus_error_free(&e);
        free(name);
        if (r < 0) break;
    }
    return n;
}

static void execute_method(sd_bus *bus, int card, const char *method, double val, int has_arg) {
    char path[64]; card_path(card, path, sizeof(path));
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r;
    if (has_arg) r = sd_bus_call_method(bus, SVC, path, IFACE, method, &error, &m, "d", val);
    else         r = sd_bus_call_method(bus, SVC, path, IFACE, method, &error, &m, "");

    if (r < 0) {
        fprintf(stderr, "Command failed: %s\n(Check the daemon: systemctl status arcd)\n", error.message);
    } else {
        int success = 0;
        sd_bus_message_read(m, "b", &success);
        printf("%s on card%d: %s\n", method, card,
               success ? "OK" : "REJECTED (driver refused, or not supported on this card)");
    }
    sd_bus_error_free(&error);
    if (m) sd_bus_message_unref(m);
}

// Print one card's telemetry block. Returns 0 if the card isn't present.
static int print_card(sd_bus *bus, int idx) {
    char path[64]; card_path(idx, path, sizeof(path));
    sd_bus_error e = SD_BUS_ERROR_NULL;
    char *gpu = NULL, *arch = NULL, *rebar = NULL;
    uint32_t eus = 0, xe = 0, fan = 0;
    uint64_t vtot = 0, vused = 0, cvis = 0;
    double temp = 0.0;
    sd_bus_message *m = NULL;

    if (sd_bus_get_property_string(bus, SVC, path, IFACE, "GpuName", &e, &gpu) < 0) {
        sd_bus_error_free(&e);
        return 0;
    }
    sd_bus_get_property_string (bus, SVC, path, IFACE, "Architecture",   &e, &arch);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "XeCores",        &e, 'u', &xe);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "ComputeCores",   &e, 'u', &eus);
    sd_bus_get_property_string (bus, SVC, path, IFACE, "RebarStatus",    &e, &rebar);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "CpuVisibleVram", &e, 't', &cvis);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "VramTotal",      &e, 't', &vtot);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "VramUsed",       &e, 't', &vused);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "Temperature",    &e, 'd', &temp);
    sd_bus_get_property_trivial(bus, SVC, path, IFACE, "FanRPM",         &e, 'u', &fan);

    printf("== card%d: %s ==\033[K\n", idx, gpu ? gpu : "?");
    printf("  Architecture : %s\033[K\n", arch ? arch : "?");
    printf("  Cores        : %u Xe Cores (%u EUs)\033[K\n", xe, eus);
    printf("  ReBAR        : %s\033[K\n", rebar ? rebar : "?");
    printf("  CPU Visible  : %.2f GB\033[K\n", cvis / (1024.0*1024.0*1024.0));
    printf("  VRAM         : %.0f / %.0f MB\033[K\n", vused/(1024.0*1024.0), vtot/(1024.0*1024.0));
    if (temp > 0.0) printf("  Temp         : %.1f \u00b0C\033[K\n", temp);
    else            printf("  Temp         : [n/a]\033[K\n");
    printf("  Fan          : %u RPM\033[K\n", fan);

    if (sd_bus_get_property(bus, SVC, path, IFACE, "Engines", &e, &m, "a(sd)") >= 0) {
        sd_bus_message_enter_container(m, 'a', "(sd)");
        const char *name; double util;
        while (sd_bus_message_read(m, "(sd)", &name, &util) > 0)
            printf("  Engine %-11s : %5.1f%%\033[K\n", name, util);
        sd_bus_message_exit_container(m);
        sd_bus_message_unref(m);
    }
    printf("\033[K\n");

    free(gpu); free(arch); free(rebar);
    sd_bus_error_free(&e);
    return 1;
}

static void monitor_mode(sd_bus *bus) {
    int n = count_cards(bus);
    if (n == 0) {
        fprintf(stderr, "No cards exposed by arcd. Is the daemon running? (systemctl status arcd)\n");
        return;
    }
    printf("\033[2J");
    while (1) {
        printf("\033[H");
        printf("=== Intel Arc Telemetry (D-Bus, %d card%s) ===\033[K\n\n", n, n == 1 ? "" : "s");
        for (int i = 0; i < n; i++) print_card(bus, i);
        printf("\033[J");
        fflush(stdout);
        usleep(500 * 1000);
    }
}

int main(int argc, char **argv) {
    sd_bus *bus = NULL;
    if (sd_bus_open_system(&bus) < 0) {
        fprintf(stderr, "Failed to connect to D-Bus system bus.\n");
        return EXIT_FAILURE;
    }

    int card = 0;
    const char *cmd = NULL;
    const char *arg = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--card") == 0 && i + 1 < argc) {
            card = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--reset-clock") == 0 || strcmp(argv[i], "--auto-fan") == 0) {
            cmd = argv[i];
        } else if ((strcmp(argv[i], "--set-pl1") == 0 || strcmp(argv[i], "--lock-clock") == 0 ||
                    strcmp(argv[i], "--set-fan") == 0) && i + 1 < argc) {
            cmd = argv[i]; arg = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            sd_bus_unref(bus);
            return EXIT_FAILURE;
        }
    }

    if (!cmd) {
        monitor_mode(bus);
    } else if (strcmp(cmd, "--set-pl1") == 0) {
        execute_method(bus, card, "SetPowerLimit", atof(arg), 1);
    } else if (strcmp(cmd, "--lock-clock") == 0) {
        execute_method(bus, card, "LockClock", atof(arg), 1);
    } else if (strcmp(cmd, "--reset-clock") == 0) {
        execute_method(bus, card, "ResetClock", 0.0, 0);
    } else if (strcmp(cmd, "--set-fan") == 0) {
        execute_method(bus, card, "SetFanPWM", atof(arg), 1);
    } else if (strcmp(cmd, "--auto-fan") == 0) {
        execute_method(bus, card, "SetFanAuto", 0.0, 0);
    }

    sd_bus_unref(bus);
    return EXIT_SUCCESS;
}
