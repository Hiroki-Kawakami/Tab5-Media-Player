/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "usb_msc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "harness.h"
#include "simulator/path_redirect.h"

#define MOUNT_POINT_MAX 64
#define HOST_DIR_MAX    1024

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_initialized;
static usb_msc_event_cb_t s_cb;
static void *s_cb_arg;

static char s_host_dir[HOST_DIR_MAX];
static bool s_connected;
static char s_mount_point[MOUNT_POINT_MAX];
static bool s_mounted;
static bool s_gone;

static void notify(usb_msc_event_t event) {
    if (s_cb) s_cb(event, s_cb_arg);
}

static bool is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static esp_err_t attach(const char *host_dir) {
    if (strlen(host_dir) >= HOST_DIR_MAX || !is_directory(host_dir)) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&s_lock);
    const bool was_connected = s_connected;
    const bool same = was_connected && strcmp(s_host_dir, host_dir) == 0;
    if (!was_connected) {
        strcpy(s_host_dir, host_dir);
        s_connected = true;
    }
    pthread_mutex_unlock(&s_lock);
    if (was_connected) return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    notify(USB_MSC_EVENT_CONNECTED);
    return ESP_OK;
}

static void detach(void) {
    pthread_mutex_lock(&s_lock);
    const bool was_connected = s_connected;
    s_connected = false;
    if (was_connected && s_mounted && !s_gone) {
        sim_path_redirect_remove(s_mount_point);
        s_gone = true;
    }
    pthread_mutex_unlock(&s_lock);
    if (was_connected) notify(USB_MSC_EVENT_DISCONNECTED);
}

static const char *default_host_dir(void) {
    const char *dir = getenv("SIMULATOR_USB_PATH");
    return dir ? dir : "simulator/usb";
}

static bool cmd_attach(int argc, const char *const *argv, void *user) {
    (void)user;
    const char *dir = argc > 1 ? argv[1] : default_host_dir();
    const esp_err_t err = attach(dir);
    if (err == ESP_ERR_INVALID_ARG) {
        harness_reply("ERR %s: not a directory: %s", argv[0], dir);
    } else if (err != ESP_OK) {
        harness_reply("ERR %s: another directory is attached", argv[0]);
    }
    return true;
}

static bool cmd_detach(int argc, const char *const *argv, void *user) {
    (void)argc;
    (void)argv;
    (void)user;
    detach();
    return true;
}

esp_err_t usb_msc_init(usb_msc_event_cb_t cb, void *arg) {
    if (s_initialized) return ESP_ERR_INVALID_STATE;
    s_initialized = true;
    s_cb = cb;
    s_cb_arg = arg;
    harness_register("usb-attach", cmd_attach, NULL);
    harness_register("usb-detach", cmd_detach, NULL);
    if (getenv("SIMULATOR_USB_PATH")) attach(default_host_dir());
    return ESP_OK;
}

bool usb_msc_is_connected(void) {
    pthread_mutex_lock(&s_lock);
    const bool connected = s_connected;
    pthread_mutex_unlock(&s_lock);
    return connected;
}

static void release_locked(void) {
    if (!s_mounted) return;
    if (!s_gone) sim_path_redirect_remove(s_mount_point);
    s_mounted = false;
    s_gone = false;
}

esp_err_t usb_msc_mount(const char *mount_point, uint8_t max_files) {
    (void)max_files;
    if (!mount_point || strlen(mount_point) >= MOUNT_POINT_MAX) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    pthread_mutex_lock(&s_lock);
    if (s_mounted && !s_gone) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        release_locked();
        if (!s_connected) {
            err = ESP_ERR_NOT_FOUND;
        } else {
            err = sim_path_redirect_add(mount_point, s_host_dir);
            if (err == ESP_OK) {
                strcpy(s_mount_point, mount_point);
                s_mounted = true;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
    return err;
}

esp_err_t usb_msc_unmount(void) {
    pthread_mutex_lock(&s_lock);
    const bool mounted = s_mounted;
    release_locked();
    pthread_mutex_unlock(&s_lock);
    return mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool usb_msc_is_mounted(void) {
    pthread_mutex_lock(&s_lock);
    const bool mounted = s_mounted && !s_gone;
    pthread_mutex_unlock(&s_lock);
    return mounted;
}
