/*
 * Copyright © 2026 dancingmirrors@icloud.com
 *
 * This file is part of lachesis.
 *
 * lachesis is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * lachesis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with lachesis; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <SDL3/SDL.h>

#include <libavutil/time.h>

#include "lachesis_config.h"

#if LACHESIS_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#include "lachesis_present_vulkan.h"
#include <vulkan/vulkan.h>
#endif

#include "lachesis_internal.h"
#include "lachesis_log.h"
#include "lachesis_options.h"
#include "lachesis_present.h"

#define MAX_VSYNC_SAMPLES 200
#define DELAY_VSYNC_SAMPLES 10
#define PRESENT_BLOCK_MIN_US 1000
#define PRESENT_MAX_GAP_US 1000000
#define PRESENT_MAX_FOLD 8
#define PRESENT_ANCHOR_STALE_US 1000000

static struct {
    double nominal_us;
    double driver_us;
    double interval_us;
    double estimated_us;
    double jitter;
    int use_estimated;

    int64_t samples[MAX_VSYNC_SAMPLES];
    int num_samples;
    int64_t num_total_samples;

    int64_t last_done_us;
    int64_t last_blocked_done_us;
    int64_t last_present_us;
    int num_successive;

    int snap_disabled;
    int source;
} pres;

const char *present_source_name(int source) {
    switch (source) {
    case PRESENT_SOURCE_PRESENT_WAIT:
        return "present-wait";
    case PRESENT_SOURCE_DISPLAY_TIMING:
        return "display-timing";
    default:
        return "swap";
    }
}

static double vsync_stddev(double ref_us) {
    double jitter = 0;
    for (int n = 0; n < pres.num_samples; n++) {
        double diff = pres.samples[n] - ref_us;
        jitter += diff * diff;
    }

    return sqrt(jitter / pres.num_samples);
}

static int enough_samples(void) {
    return pres.num_total_samples >= MAX_VSYNC_SAMPLES / 2;
}

static void check_estimated_display_fps(void) {
    int use_estimated = 0;

    if (enough_samples() &&
        pres.estimated_us <= 1e6 / 20.0 && pres.estimated_us >= 1e6 / 400.0) {
        use_estimated = 1;
        for (int n = 0; n < pres.num_samples; n++) {
            if (fabs(pres.samples[n] - pres.estimated_us) >= pres.estimated_us / 4) {
                use_estimated = 0;
                break;
            }
        }
        if (use_estimated && pres.nominal_us > 0) {
            double mjitter = vsync_stddev(pres.estimated_us);
            double njitter = vsync_stddev(pres.nominal_us);
            if (mjitter * 1.01 >= njitter) {
                use_estimated = 0;
            }
        }
    }

    if (use_estimated != pres.use_estimated) {
        if (use_estimated) {
            log_verbose("Adjusting display FPS to a measured %.3f Hz.\n",
                        1e6 / pres.estimated_us);
        } else if (pres.nominal_us > 0) {
            log_verbose("Switching back to the reported display FPS of %.3f Hz.\n",
                        1e6 / pres.nominal_us);
        }
        pres.use_estimated = use_estimated;
    }

    pres.interval_us = pres.use_estimated ? pres.estimated_us : pres.nominal_us;
}

void present_update_display_mode(void) {
    double hz = 0;

    if (display_fps_override > 0) {
        hz = display_fps_override;
    } else if (pres.driver_us > 0) {
        hz = 1e6 / pres.driver_us;
    } else if (window) {
        SDL_DisplayID id = SDL_GetDisplayForWindow(window);
        const SDL_DisplayMode *mode = id ? SDL_GetCurrentDisplayMode(id) : NULL;
        if (mode) {
            if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0) {
                hz = (double)mode->refresh_rate_numerator / mode->refresh_rate_denominator;
            } else if (mode->refresh_rate > 0) {
                hz = mode->refresh_rate;
            }
        }
    }

    double nominal_us = hz > 0 ? 1e6 / hz : 0;
    if (nominal_us != pres.nominal_us) {
        pres.nominal_us = nominal_us;
        pres.num_samples = 0;
        pres.num_total_samples = 0;
        pres.estimated_us = 0;
        pres.use_estimated = 0;
        pres.jitter = 0;
        present_reset();
        check_estimated_display_fps();
    }
}

void present_set_refresh_interval(double refresh_us) {
    if (refresh_us > 1e6 / 20.0 || refresh_us < 1e6 / 400.0) {
        refresh_us = 0;
    }
    if (refresh_us == pres.driver_us) {
        return;
    }
    if (refresh_us > 0) {
        log_verbose("The driver reports a display refresh of %.3f Hz.\n",
                    1e6 / refresh_us);
    }
    pres.driver_us = refresh_us;
    present_update_display_mode();
}

void present_disable_snap(void) {
    pres.snap_disabled = 1;
}

void present_restore_snap(void) {
    pres.snap_disabled = 0;
}

void present_reset(void) {
    pres.num_successive = 0;
    pres.last_done_us = 0;
    pres.last_blocked_done_us = 0;
    pres.last_present_us = 0;
}

void present_note_present(int64_t done_us) {
    if (done_us > pres.last_present_us) {
        pres.last_present_us = done_us;
    }
}

static void feedback_sample(int64_t done_us, int blocked) {
    int64_t prev_done = pres.last_done_us;
    int64_t prev_blocked = pres.last_blocked_done_us;

    if (done_us <= 0) {
        return;
    }
    pres.last_done_us = done_us;
    if (blocked) {
        pres.last_blocked_done_us = done_us;
    }

    if (prev_done <= 0 || done_us - prev_done > PRESENT_MAX_GAP_US) {
        pres.num_successive = 0;
        return;
    }

    pres.num_successive++;
    if (pres.num_successive <= DELAY_VSYNC_SAMPLES) {
        return;
    }

    if (!blocked || prev_blocked <= 0 || done_us - prev_blocked > PRESENT_MAX_GAP_US) {
        return;
    }

    double ref_us = pres.nominal_us > 0 ? pres.nominal_us : pres.interval_us;
    int64_t delta = done_us - prev_blocked;
    int64_t folds = 1;
    if (ref_us > 0) {
        folds = llrint(delta / ref_us);
        if (folds < 1 || folds > PRESENT_MAX_FOLD) {
            return;
        }
        if (fabs(delta - folds * ref_us) >= ref_us / 4) {
            return;
        }
    } else if (delta < 1e6 / 400.0 || delta > 1e6 / 20.0) {
        return;
    }

    if (pres.num_samples >= MAX_VSYNC_SAMPLES) {
        pres.num_samples -= 1;
    }
    memmove(&pres.samples[1], &pres.samples[0],
            pres.num_samples * sizeof(pres.samples[0]));
    pres.samples[0] = (int64_t)llrint((double)delta / folds);
    pres.num_samples++;
    pres.num_total_samples++;

    double avg = 0;
    for (int n = 0; n < pres.num_samples; n++) {
        avg += pres.samples[n];
    }
    pres.estimated_us = avg / pres.num_samples;

    check_estimated_display_fps();

    if (pres.interval_us > 0) {
        pres.jitter = vsync_stddev(pres.interval_us) / pres.interval_us;
    }
}

static void switch_source(int source) {
    if (source == pres.source) {
        return;
    }
    pres.source = source;
    if (source == PRESENT_SOURCE_SWAP) {
        present_set_refresh_interval(0);
    }

    present_reset();
}

void present_feedback(int64_t submit_us, int64_t done_us) {
    if (done_us < submit_us) {
        return;
    }
    switch_source(PRESENT_SOURCE_SWAP);
    present_note_present(done_us);
    feedback_sample(done_us, done_us - submit_us >= PRESENT_BLOCK_MIN_US);
}

void present_feedback_display(int source, int64_t display_us, double refresh_us) {
    if (source == PRESENT_SOURCE_SWAP || display_us <= 0) {
        return;
    }
    switch_source(source);
    present_set_refresh_interval(refresh_us);
    feedback_sample(display_us, 1);
}

double present_vsync_sec(void) {
    return pres.interval_us > 0 ? pres.interval_us / 1e6 : 0;
}

int64_t present_last_done_us(void) {
    return pres.last_present_us;
}

double present_snap(double ideal_sec, double now_sec) {
    double vsync = present_vsync_sec();

    if (pres.snap_disabled || vsync <= 0 || pres.last_blocked_done_us <= 0) {
        return ideal_sec;
    }
    if (ideal_sec <= now_sec) {
        return ideal_sec;
    }

    double anchor = pres.last_blocked_done_us / 1e6;
    if (now_sec - anchor > PRESENT_ANCHOR_STALE_US / 1e6) {
        return ideal_sec;
    }

    double target = anchor + round((ideal_sec - anchor) / vsync) * vsync;
    if (target <= now_sec) {
        return ideal_sec;
    }

    return target;
}

void present_get_stats(PresentStats *st) {
    memset(st, 0, sizeof(*st));
    st->nominal_hz = pres.nominal_us > 0 ? 1e6 / pres.nominal_us : 0;
    if (pres.estimated_us > 0 && enough_samples()) {
        st->measured_hz = 1e6 / pres.estimated_us;
    }
    st->samples_needed = MAX_VSYNC_SAMPLES / 2;
    st->samples = pres.num_total_samples >= st->samples_needed
        ? st->samples_needed
        : (int)pres.num_total_samples;
    st->jitter = pres.jitter;
    st->measuring = pres.use_estimated;
    st->snapping = !pres.snap_disabled && pres.interval_us > 0;
    st->source = pres.source;
    st->driver_refresh = pres.driver_us > 0;
}

#if LACHESIS_HAVE_VULKAN
#define WAIT_TIMEOUT_NS UINT64_C(50000000)
#define MAX_PAST_TIMINGS 16
#define PRESENT_ID_RING 16
#if defined(VK_KHR_present_id) && defined(VK_KHR_present_wait)
#define HAVE_PRESENT_WAIT 1
#else
#define HAVE_PRESENT_WAIT 0
#endif
#if defined(VK_GOOGLE_display_timing) && defined(CLOCK_MONOTONIC) && !defined(_WIN32)
#define HAVE_DISPLAY_TIMING 1
#else
#define HAVE_DISPLAY_TIMING 0
#endif

static struct {
    int installed;

    PFN_vkGetInstanceProcAddr real_instance_proc_addr;
    PFN_vkGetDeviceProcAddr real_device_proc_addr;

    PFN_vkCreateSwapchainKHR real_create_swapchain;
    PFN_vkDestroySwapchainKHR real_destroy_swapchain;
    PFN_vkQueuePresentKHR real_queue_present;

#if HAVE_PRESENT_WAIT
    PFN_vkWaitForPresentKHR wait_for_present;
#endif
#if HAVE_DISPLAY_TIMING
    PFN_vkGetPastPresentationTimingGOOGLE get_past_timing;
    PFN_vkGetRefreshCycleDurationGOOGLE get_refresh_cycle;
#endif

    VkDevice device;

    int have_present_wait;
    int have_display_timing;
    int device_lost;

    SDL_Mutex *lock;
    SDL_Condition *cond;
    SDL_Thread *thread;
    int quit;

    VkSwapchainKHR swapchain;
    uint64_t generation;
    uint64_t next_id;
    uint64_t submitted[PRESENT_ID_RING];
    unsigned submitted_head;
    unsigned submitted_tail;

    int waiting;
    VkSwapchainKHR waiting_on;

    int64_t wait_sample_us;
    uint64_t wait_sample_seq;
    uint64_t wait_taken_seq;

    uint64_t timing_generation;
    double refresh_us;
} vkp;

av_unused static int64_t driver_ns_to_relative_us(uint64_t driver_ns) {
#if HAVE_DISPLAY_TIMING
    struct timespec ts;

    if (driver_ns && !clock_gettime(CLOCK_MONOTONIC, &ts)) {
        int64_t now_ns = (int64_t)ts.tv_sec * INT64_C(1000000000) + ts.tv_nsec;
        int64_t behind_us = (now_ns - (int64_t)driver_ns) / 1000;

        if (behind_us > -1000 && behind_us < 1000000) {
            return av_gettime_relative() - (behind_us > 0 ? behind_us : 0);
        }
    }
#else
    (void)driver_ns;
#endif
    return 0;
}

/* Both called with the lock held. */
static int have_submitted(void) {
    return vkp.submitted_tail != vkp.submitted_head;
}

static void drop_submitted(void) {
    vkp.submitted_tail = vkp.submitted_head;
}

#if HAVE_PRESENT_WAIT
static int waiter_thread(void *unused) {
    (void)unused;

    for (;;) {
        VkSwapchainKHR swapchain;
        uint64_t generation;
        uint64_t id;
        VkResult res;
        int64_t now;

        SDL_LockMutex(vkp.lock);
        while (!vkp.quit &&
               (vkp.swapchain == VK_NULL_HANDLE || !have_submitted())) {
            SDL_WaitCondition(vkp.cond, vkp.lock);
        }
        if (vkp.quit) {
            SDL_UnlockMutex(vkp.lock);
            break;
        }
        swapchain = vkp.swapchain;
        generation = vkp.generation;
        id = vkp.submitted[vkp.submitted_tail];
        vkp.waiting = 1;
        vkp.waiting_on = swapchain;
        SDL_UnlockMutex(vkp.lock);

        res = vkp.wait_for_present(vkp.device, swapchain, id, WAIT_TIMEOUT_NS);
        now = av_gettime_relative();

        SDL_LockMutex(vkp.lock);
        vkp.waiting = 0;
        vkp.waiting_on = VK_NULL_HANDLE;
        if (generation == vkp.generation) {
            if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
                vkp.submitted_tail = (vkp.submitted_tail + 1) % PRESENT_ID_RING;
                vkp.wait_sample_us = now;
                vkp.wait_sample_seq++;
            } else if (res == VK_ERROR_DEVICE_LOST) {
                drop_submitted();
                vkp.device_lost = 1;
                vkp.quit = 1;
            } else if (res != VK_TIMEOUT) {
                drop_submitted();
            }
        }
        SDL_BroadcastCondition(vkp.cond);
        SDL_UnlockMutex(vkp.lock);
    }

    return 0;
}
#endif /* HAVE_PRESENT_WAIT */

static VKAPI_ATTR VkResult VKAPI_CALL
hook_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                      const VkAllocationCallbacks *alloc,
                      VkSwapchainKHR *swapchain) {
    VkResult res;

    SDL_LockMutex(vkp.lock);
    vkp.swapchain = VK_NULL_HANDLE;
    vkp.generation++;
    drop_submitted();
    while (vkp.waiting) {
        SDL_WaitCondition(vkp.cond, vkp.lock);
    }
    SDL_UnlockMutex(vkp.lock);

    res = vkp.real_create_swapchain(device, info, alloc, swapchain);
    if (res != VK_SUCCESS) {
        return res;
    }

    SDL_LockMutex(vkp.lock);
    vkp.swapchain = *swapchain;
    vkp.generation++;
    vkp.next_id = 0;
    vkp.wait_sample_us = 0;
    SDL_BroadcastCondition(vkp.cond);
    SDL_UnlockMutex(vkp.lock);

    return res;
}

static VKAPI_ATTR void VKAPI_CALL
hook_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                       const VkAllocationCallbacks *alloc) {
    SDL_LockMutex(vkp.lock);
    if (vkp.swapchain == swapchain) {
        vkp.swapchain = VK_NULL_HANDLE;
        vkp.generation++;
        drop_submitted();
    }
    while (vkp.waiting && vkp.waiting_on == swapchain) {
        SDL_WaitCondition(vkp.cond, vkp.lock);
    }
    SDL_UnlockMutex(vkp.lock);

    vkp.real_destroy_swapchain(device, swapchain, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL
hook_queue_present(VkQueue queue, const VkPresentInfoKHR *info) {
#if !HAVE_PRESENT_WAIT
    return vkp.real_queue_present(queue, info);
#else
    VkPresentInfoKHR tagged;
    VkPresentIdKHR present_id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
    };
    uint64_t id;
    VkResult res;

    if (!vkp.have_present_wait || info->swapchainCount != 1) {
        return vkp.real_queue_present(queue, info);
    }

    SDL_LockMutex(vkp.lock);
    id = info->pSwapchains[0] == vkp.swapchain ? ++vkp.next_id : 0;
    SDL_UnlockMutex(vkp.lock);

    if (!id) {
        return vkp.real_queue_present(queue, info);
    }

    present_id.pPresentIds = &id;
    present_id.pNext = info->pNext;
    tagged = *info;
    tagged.pNext = &present_id;

    res = vkp.real_queue_present(queue, &tagged);

    if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
        SDL_LockMutex(vkp.lock);
        if (vkp.swapchain == info->pSwapchains[0]) {
            unsigned next = (vkp.submitted_head + 1) % PRESENT_ID_RING;

            if (next == vkp.submitted_tail) {
                vkp.submitted_tail = (vkp.submitted_tail + 1) % PRESENT_ID_RING;
            }
            vkp.submitted[vkp.submitted_head] = id;
            vkp.submitted_head = next;
            SDL_BroadcastCondition(vkp.cond);
        }
        SDL_UnlockMutex(vkp.lock);
    }

    return res;
#endif /* HAVE_PRESENT_WAIT */
}

static PFN_vkVoidFunction hook_lookup(const char *name) {
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        return (PFN_vkVoidFunction)hook_create_swapchain;
    }
    if (!strcmp(name, "vkDestroySwapchainKHR")) {
        return (PFN_vkVoidFunction)hook_destroy_swapchain;
    }
    if (!strcmp(name, "vkQueuePresentKHR")) {
        return (PFN_vkVoidFunction)hook_queue_present;
    }

    return NULL;
}

static void hook_remember(const char *name, PFN_vkVoidFunction real) {
    if (!strcmp(name, "vkCreateSwapchainKHR")) {
        vkp.real_create_swapchain = (PFN_vkCreateSwapchainKHR)real;
    } else if (!strcmp(name, "vkDestroySwapchainKHR")) {
        vkp.real_destroy_swapchain = (PFN_vkDestroySwapchainKHR)real;
    } else if (!strcmp(name, "vkQueuePresentKHR")) {
        vkp.real_queue_present = (PFN_vkQueuePresentKHR)real;
    }
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
hook_device_proc_addr(VkDevice device, const char *name) {
    PFN_vkVoidFunction real;

    if (!vkp.real_device_proc_addr) {
        return NULL;
    }
    real = vkp.real_device_proc_addr(device, name);
    PFN_vkVoidFunction hook;

    if (!real || !name) {
        return real;
    }
    hook_remember(name, real);
    hook = hook_lookup(name);

    return hook ? hook : real;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
hook_instance_proc_addr(VkInstance instance, const char *name) {
    PFN_vkVoidFunction real;

    if (!vkp.real_instance_proc_addr) {
        return NULL;
    }
    real = vkp.real_instance_proc_addr(instance, name);
    PFN_vkVoidFunction hook;

    if (!real || !name) {
        return real;
    }
    if (!strcmp(name, "vkGetDeviceProcAddr")) {
        vkp.real_device_proc_addr = (PFN_vkGetDeviceProcAddr)real;
        return (PFN_vkVoidFunction)hook_device_proc_addr;
    }
    hook_remember(name, real);
    hook = hook_lookup(name);

    return hook ? hook : real;
}

PFN_vkGetInstanceProcAddr vkpresent_wrap_proc_addr(PFN_vkGetInstanceProcAddr real) {
    if (!real) {
        return real;
    }
    if (vkp.installed) {
        return vkp.real_instance_proc_addr == real ? hook_instance_proc_addr : real;
    }

    vkp.lock = SDL_CreateMutex();
    vkp.cond = SDL_CreateCondition();
    if (!vkp.lock || !vkp.cond) {
        SDL_DestroyCondition(vkp.cond);
        SDL_DestroyMutex(vkp.lock);
        memset(&vkp, 0, sizeof(vkp));
        return real;
    }
    vkp.real_instance_proc_addr = real;
    vkp.installed = 1;

    return hook_instance_proc_addr;
}

const char *const *vkpresent_device_extensions(int *count) {
    static const char *const extensions[] = {
#if HAVE_PRESENT_WAIT
        VK_KHR_PRESENT_ID_EXTENSION_NAME,
        VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
#endif
#if HAVE_DISPLAY_TIMING
        VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME,
#endif
        NULL,
    };
    *count = (int)(sizeof(extensions) / sizeof(extensions[0])) - 1;

    return extensions;
}

const VkPhysicalDeviceFeatures2 *vkpresent_device_features(void) {
#if !HAVE_PRESENT_WAIT
    return NULL;
#else
    static VkPhysicalDevicePresentIdFeaturesKHR present_id = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,
        .presentId = VK_TRUE,
    };
    static VkPhysicalDevicePresentWaitFeaturesKHR present_wait = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
        .pNext = &present_id,
        .presentWait = VK_TRUE,
    };
    static VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &present_wait,
    };

    return &features;
#endif
}

static int has_extension(const char *const *extensions, int num_extensions,
                         const char *name) {
    for (int i = 0; i < num_extensions; i++) {
        if (extensions[i] && !strcmp(extensions[i], name)) {
            return 1;
        }
    }

    return 0;
}

static int features_enabled(const VkPhysicalDeviceFeatures2 *features) {
#if !HAVE_PRESENT_WAIT
    (void)features;
    return 0;
#else
    int id = 0;
    int wait = 0;

    for (const VkBaseInStructure *s = (const VkBaseInStructure *)features; s;
         s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR) {
            id = ((const VkPhysicalDevicePresentIdFeaturesKHR *)s)->presentId;
        } else if (s->sType ==
                   VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR) {
            wait = ((const VkPhysicalDevicePresentWaitFeaturesKHR *)s)->presentWait;
        }
    }

    return id && wait;
#endif
}

void vkpresent_attach(VkDevice device, const char *const *extensions,
                      int num_extensions,
                      const VkPhysicalDeviceFeatures2 *features) {
    if (!vkp.installed || vkp.device || !device || !vkp.real_device_proc_addr) {
        return;
    }

    vkp.device = device;

#if HAVE_DISPLAY_TIMING
    if (has_extension(extensions, num_extensions,
                      VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
        vkp.get_past_timing = (PFN_vkGetPastPresentationTimingGOOGLE)
                                  vkp.real_device_proc_addr(device, "vkGetPastPresentationTimingGOOGLE");
        vkp.get_refresh_cycle = (PFN_vkGetRefreshCycleDurationGOOGLE)
                                    vkp.real_device_proc_addr(device, "vkGetRefreshCycleDurationGOOGLE");
        vkp.have_display_timing = vkp.get_past_timing != NULL;
    }
#endif

#if HAVE_PRESENT_WAIT
    if (!vkp.have_display_timing &&
        has_extension(extensions, num_extensions,
                      VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
        has_extension(extensions, num_extensions,
                      VK_KHR_PRESENT_WAIT_EXTENSION_NAME) &&
        features_enabled(features)) {
        vkp.wait_for_present = (PFN_vkWaitForPresentKHR)
                                   vkp.real_device_proc_addr(device, "vkWaitForPresentKHR");
        vkp.have_present_wait = vkp.wait_for_present != NULL;
    }
#endif

    if (vkp.have_present_wait) {
        vkp.thread = SDL_CreateThread(waiter_thread, "vk_present_wait", NULL);
        if (!vkp.thread) {
            log_warn("Failed to start the presentation feedback thread: %s.\n",
                     SDL_GetError());
            vkp.have_present_wait = 0;
        }
    }

    if (vkp.have_display_timing) {
        log_verbose("Presentation feedback via VK_GOOGLE_display_timing.\n");
    } else if (vkp.have_present_wait) {
        log_verbose("Presentation feedback via VK_KHR_present_wait.\n");
    } else {
        log_verbose("No presentation timing extension is available. Falling "
                    "back to timing the swap on the CPU.\n");
    }
}

int vkpresent_source(void) {
    if (vkp.have_display_timing) {
        return PRESENT_SOURCE_DISPLAY_TIMING;
    }
    if (vkp.have_present_wait) {
        return PRESENT_SOURCE_PRESENT_WAIT;
    }

    return PRESENT_SOURCE_SWAP;
}

#if HAVE_DISPLAY_TIMING
static void refresh_refresh_cycle(VkSwapchainKHR swapchain, uint64_t generation) {
    VkRefreshCycleDurationGOOGLE cycle = {0};

    if (!vkp.get_refresh_cycle || generation == vkp.timing_generation) {
        return;
    }
    vkp.timing_generation = generation;
    vkp.refresh_us = 0;

    if (vkp.get_refresh_cycle(vkp.device, swapchain, &cycle) == VK_SUCCESS &&
        cycle.refreshDuration) {
        vkp.refresh_us = (double)cycle.refreshDuration / 1000.0;
    }
}

static int poll_display_timing(VkSwapchainKHR swapchain, VkPresentSample *out) {
    uint64_t newest_ns = 0;
    int64_t display_us;

    for (int round = 0; round < 8; round++) {
        VkPastPresentationTimingGOOGLE timings[MAX_PAST_TIMINGS];
        uint32_t count = MAX_PAST_TIMINGS;
        VkResult res = vkp.get_past_timing(vkp.device, swapchain, &count,
                                           timings);

        if (res != VK_SUCCESS && res != VK_INCOMPLETE) {
            if (res == VK_ERROR_DEVICE_LOST) {
                vkp.device_lost = 1;
            }
            break;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (timings[i].actualPresentTime > newest_ns) {
                newest_ns = timings[i].actualPresentTime;
            }
        }
        if (res == VK_SUCCESS || !count) {
            break;
        }
    }

    display_us = driver_ns_to_relative_us(newest_ns);
    if (display_us <= 0) {
        return 0;
    }

    out->display_us = display_us;
    out->refresh_us = vkp.refresh_us;
    out->source = PRESENT_SOURCE_DISPLAY_TIMING;

    return 1;
}
#endif /* HAVE_DISPLAY_TIMING */

static int poll_present_wait(VkPresentSample *out) {
    int64_t display_us = 0;

    SDL_LockMutex(vkp.lock);
    if (vkp.wait_sample_seq != vkp.wait_taken_seq) {
        vkp.wait_taken_seq = vkp.wait_sample_seq;
        display_us = vkp.wait_sample_us;
    }
    SDL_UnlockMutex(vkp.lock);

    if (display_us <= 0) {
        return 0;
    }

    out->display_us = display_us;
    out->refresh_us = vkp.refresh_us;
    out->source = PRESENT_SOURCE_PRESENT_WAIT;

    return 1;
}

int vkpresent_poll(VkPresentSample *out) {
    VkSwapchainKHR swapchain;
    uint64_t generation;

    memset(out, 0, sizeof(*out));

    if (!vkp.device || (!vkp.have_display_timing && !vkp.have_present_wait)) {
        return 0;
    }

    SDL_LockMutex(vkp.lock);
    swapchain = vkp.swapchain;
    generation = vkp.generation;
    if (vkp.device_lost) {
        swapchain = VK_NULL_HANDLE;
    }
    SDL_UnlockMutex(vkp.lock);

    if (vkp.device_lost && (vkp.have_display_timing || vkp.have_present_wait)) {
        log_warn("The Vulkan device was lost. Giving up on presentation "
                 "feedback.\n");
        vkpresent_disable();
        return 0;
    }

    if (swapchain == VK_NULL_HANDLE) {
        return 0;
    }

#if HAVE_DISPLAY_TIMING
    refresh_refresh_cycle(swapchain, generation);
    if (vkp.have_display_timing && poll_display_timing(swapchain, out)) {
        return 1;
    }
#else
    (void)generation;
#endif
    if (vkp.have_present_wait && poll_present_wait(out)) {
        return 1;
    }

    return 0;
}

static void stop_waiter(void) {
    if (!vkp.thread) {
        return;
    }

    SDL_LockMutex(vkp.lock);
    vkp.quit = 1;
    SDL_BroadcastCondition(vkp.cond);
    SDL_UnlockMutex(vkp.lock);
    SDL_WaitThread(vkp.thread, NULL);
    vkp.thread = NULL;
    SDL_LockMutex(vkp.lock);
    vkp.quit = 0;
    SDL_UnlockMutex(vkp.lock);
}

void vkpresent_disable(void) {
    if (!vkp.installed) {
        return;
    }

    stop_waiter();
    vkp.have_present_wait = 0;
    vkp.have_display_timing = 0;
}

void vkpresent_shutdown(void) {
    if (!vkp.installed) {
        return;
    }

    stop_waiter();

    SDL_DestroyCondition(vkp.cond);
    SDL_DestroyMutex(vkp.lock);
    memset(&vkp, 0, sizeof(vkp));
}

#endif /* LACHESIS_HAVE_VULKAN */
