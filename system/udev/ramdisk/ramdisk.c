// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/completion.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/block.h>

#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <magenta/listnode.h>
#include <sys/param.h>
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <threads.h>

mx_driver_t _driver_ramdisk;
static mx_device_t* ramdisk_ctl_dev;

typedef struct ram_device {
    mx_device_t device;
    uint64_t blk_size;
    uint64_t blk_count;
    mx_handle_t vmo;
    uintptr_t mapped_addr;
} ram_device_t;

#define get_ram_device(dev) containerof(dev, ram_device_t, device)

static uint64_t sizebytes(ram_device_t* rdev) {
    return rdev->blk_size * rdev->blk_count;
}

// implement device protocol:

static ssize_t ramdisk_ioctl(mx_device_t* dev, uint32_t op, const void* cmd,
                           size_t cmdlen, void* reply, size_t max) {
    ram_device_t* ramdev = get_ram_device(dev);

    switch (op) {
    case IOCTL_BLOCK_RAMDISK_CONFIG: {
        if (cmdlen != sizeof(ramdisk_ioctl_config_t)) {
            return ERR_INVALID_ARGS;
        } else if (ramdev->vmo != MX_HANDLE_INVALID) {
            return ERR_ALREADY_BOUND;
        }
        ramdisk_ioctl_config_t* config = (ramdisk_ioctl_config_t*)cmd;
        ramdev->blk_size = config->blk_size;
        ramdev->blk_count = config->blk_count;
        mx_status_t status;
        if ((status = mx_vmo_create(sizebytes(ramdev), 0, &ramdev->vmo)) != NO_ERROR) {
            return status;
        }
        if ((status = mx_vmar_map(mx_vmar_root_self(), 0, ramdev->vmo, 0, sizebytes(ramdev),
                                  MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE,
                                  &ramdev->mapped_addr)) != NO_ERROR) {
            mx_handle_close(ramdev->vmo);
            ramdev->vmo = MX_HANDLE_INVALID;
            return status;
        }
        return NO_ERROR;
    }
    case IOCTL_BLOCK_GET_SIZE: {
        uint64_t* size = reply;
        if (max < sizeof(*size)) return ERR_BUFFER_TOO_SMALL;
        *size = sizebytes(ramdev);
        return sizeof(*size);
    }
    case IOCTL_BLOCK_GET_BLOCKSIZE: {
        uint64_t* blksize = reply;
        if (max < sizeof(*blksize)) return ERR_BUFFER_TOO_SMALL;
        *blksize = ramdev->blk_size;
        return sizeof(*blksize);
    }
    case IOCTL_BLOCK_RR_PART: {
        // rebind to reread the partition table
        return device_rebind(dev);
    }
    case IOCTL_DEVICE_SYNC: {
        // Wow, we sync so quickly!
        return NO_ERROR;
    }
    default:
        return ERR_NOT_SUPPORTED;
    }
}

static void ramdisk_iotxn_queue(mx_device_t* dev, iotxn_t* txn) {
    ram_device_t* ramdev = get_ram_device(dev);

    // Offset must be aligned
    if (txn->offset % ramdev->blk_size != 0) {
        txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }

    // Constrain to device capacity
    txn->length = MIN(txn->length, sizebytes(ramdev) - txn->offset);

    // Length must be aligned
    if (txn->length % ramdev->blk_size != 0) {
        txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
        return;
    }

    switch (txn->opcode) {
        case IOTXN_OP_READ: {
            txn->ops->copyto(txn, (void*) ramdev->mapped_addr + txn->offset, txn->length, 0);
            txn->ops->complete(txn, NO_ERROR, txn->length);
            return;
        }
        case IOTXN_OP_WRITE: {
            txn->ops->copyfrom(txn, (void*) ramdev->mapped_addr + txn->offset, txn->length, 0);
            txn->ops->complete(txn, NO_ERROR, txn->length);
            return;
        }
        default: {
            txn->ops->complete(txn, ERR_INVALID_ARGS, 0);
            return;
        }
    }
}

static mx_off_t ramdisk_getsize(mx_device_t* dev) {
    return sizebytes(get_ram_device(dev));
}

static void ramdisk_unbind(mx_device_t* dev) {
    device_remove(dev);
}

static mx_status_t ramdisk_release(mx_device_t* dev) {
    ram_device_t* device = get_ram_device(dev);
    if (device->vmo != MX_HANDLE_INVALID) {
        mx_handle_close(device->vmo);
    }
    device->vmo = MX_HANDLE_INVALID;
    free(device);
    return NO_ERROR;
}

static mx_protocol_device_t ramdisk_instance_proto = {
    .ioctl = ramdisk_ioctl,
    .iotxn_queue = ramdisk_iotxn_queue,
    .get_size = ramdisk_getsize,
    .unbind = ramdisk_unbind,
    .release = ramdisk_release,
};

static mx_status_t ramdisk_open(mx_device_t* dev, mx_device_t** dev_out, uint32_t flags) {
    ram_device_t* device = calloc(1, sizeof(ram_device_t));
    if (!device) {
        return ERR_NO_MEMORY;
    }
    device_init(&device->device, &_driver_ramdisk, "ramdisk", &ramdisk_instance_proto);
    device->device.protocol_id = MX_PROTOCOL_BLOCK;
    mx_status_t status;

    if ((status = device_add_instance(&device->device, dev)) != NO_ERROR) {
        free(device);
        return status;
    }
    *dev_out = &device->device;
    return NO_ERROR;
}

static mx_protocol_device_t ramdisk_ctl_proto = {
    .open = ramdisk_open,
};

static mx_status_t ramdisk_init(mx_driver_t* driver) {
    if (device_create(&ramdisk_ctl_dev, driver, "ramdisk", &ramdisk_ctl_proto) == NO_ERROR) {
        if (device_add(ramdisk_ctl_dev, driver_get_misc_device()) < 0) {
            free(ramdisk_ctl_dev);
        }
    }
    return NO_ERROR;
}

mx_driver_t _driver_ramdisk= {
    .ops = {
        .init = ramdisk_init,
    },
};

MAGENTA_DRIVER_BEGIN(_driver_ramdisk, "ramdisk", "magenta", "0.1", 0)
MAGENTA_DRIVER_END(_driver_ramdisk)
