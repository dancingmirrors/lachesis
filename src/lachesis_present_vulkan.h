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

#ifndef LACHESIS_PRESENT_VULKAN_H
#define LACHESIS_PRESENT_VULKAN_H

#include "lachesis_config.h"

#if LACHESIS_HAVE_VULKAN

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "lachesis_present.h"

typedef struct VkPresentSample {
    int64_t display_us;
    double refresh_us;
    int source;
} VkPresentSample;

PFN_vkGetInstanceProcAddr vkpresent_wrap_proc_addr(PFN_vkGetInstanceProcAddr real);

const char *const *vkpresent_device_extensions(int *count);
const VkPhysicalDeviceFeatures2 *vkpresent_device_features(void);

void vkpresent_attach(VkDevice device, const char *const *extensions,
                      int num_extensions,
                      const VkPhysicalDeviceFeatures2 *features);

void vkpresent_force_opaque(int enable);

int vkpresent_source(void);
int vkpresent_poll(VkPresentSample *out);

void vkpresent_disable(void);
void vkpresent_shutdown(void);

#endif /* LACHESIS_HAVE_VULKAN */

#endif /* LACHESIS_PRESENT_VULKAN_H */
