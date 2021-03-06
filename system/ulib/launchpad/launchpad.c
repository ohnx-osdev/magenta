// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <launchpad/launchpad.h>
#include "elf.h"

#include <magenta/assert.h>
#include <magenta/processargs.h>
#include <magenta/stack.h>
#include <magenta/syscalls.h>
#include <mxio/loader-service.h>
#include <mxio/io.h>
#include <mxio/util.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <threads.h>

enum special_handles {
    HND_LOADER_SVC,
    HND_EXEC_VMO,
    HND_SPECIAL_COUNT
};

struct launchpad {
    uint32_t argc;
    uint32_t envc;
    char* args;
    size_t args_len;
    char* env;
    size_t env_len;

    mx_handle_t* handles;
    uint32_t* handles_info;
    size_t handle_count;
    size_t handle_alloc;

    const char* errmsg;
    mx_status_t error;

    mx_vaddr_t entry;
    mx_vaddr_t base;
    mx_vaddr_t vdso_base;

    size_t stack_size;

    mx_handle_t special_handles[HND_SPECIAL_COUNT];
    bool loader_message;
};

// Returned when calloc() fails on create, so callers
// can still call all the various add handles functions
// which will discard the handles, etc, etc.
static launchpad_t invalid_launchpad = {
    .errmsg = "create: could not allocate launchpad_t",
    .error = ERR_NO_MEMORY,
};

static mx_status_t lp_error(launchpad_t* lp, mx_status_t error, const char* msg) {
    if (lp->error == NO_ERROR) {
        lp->error = error;
        lp->errmsg = msg;
    }
    return lp->error;
}

mx_status_t launchpad_get_status(launchpad_t* lp) {
    return lp->error;
}

void launchpad_abort(launchpad_t* lp, mx_status_t error, const char* msg) {
    lp_error(lp, (error < 0) ? error : ERR_INTERNAL, msg);
}

const char* launchpad_error_message(launchpad_t* lp) {
    return lp->errmsg;
}

#define HND_LOADER_COUNT 3
// We always install the process handle as the first in the message.
#define lp_proc(lp) ((lp)->handles[0])
// We always install the vmar handle as the second in the message.
#define lp_vmar(lp) ((lp)->handles[1])

static void close_handles(mx_handle_t* handles, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (handles[i] != MX_HANDLE_INVALID) {
            mx_handle_close(handles[i]);
        }
    }
}

void launchpad_destroy(launchpad_t* lp) {
    if (lp == &invalid_launchpad)
        return;
    close_handles(lp->special_handles, HND_SPECIAL_COUNT);
    close_handles(lp->handles, lp->handle_count);
    free(lp->handles);
    free(lp->handles_info);
    free(lp->args);
    free(lp->env);
    free(lp);
}

mx_status_t launchpad_create_with_process(mx_handle_t proc,
                                          mx_handle_t vmar,
                                          launchpad_t** result) {
    launchpad_t* lp = calloc(1, sizeof(*lp));
    if (lp == NULL) {
        lp = &invalid_launchpad;
    } else {
        lp->errmsg = "no error";
        lp->stack_size = MAGENTA_DEFAULT_STACK_SIZE;
    }

    launchpad_add_handle(lp, proc, MX_HND_TYPE_PROC_SELF);
    launchpad_add_handle(lp, vmar, MX_HND_TYPE_VMAR_ROOT);

    *result = lp;
    return lp->error;
}

// Create a new process and a launchpad that will set it up.
mx_status_t launchpad_create_with_jobs(mx_handle_t creation_job, mx_handle_t transfered_job,
                                       const char* name, launchpad_t** result) {
    uint32_t name_len = strlen(name);

    mx_handle_t proc = MX_HANDLE_INVALID;
    mx_handle_t vmar = MX_HANDLE_INVALID;
    mx_status_t status = mx_process_create(creation_job, name, name_len, 0, &proc, &vmar);

    launchpad_t* lp;
    launchpad_create_with_process(proc, vmar, &lp);

    if (status < 0)
        lp_error(lp, status, "create: mx_process_create() failed");

    if (transfered_job != MX_HANDLE_INVALID) {
        launchpad_add_handle(lp, transfered_job, MX_HND_INFO(MX_HND_TYPE_JOB, 0));
    }

    *result = lp;
    return lp->error;
}

mx_status_t launchpad_create(mx_handle_t job,
                             const char* name, launchpad_t** result) {
    if (job == MX_HANDLE_INVALID)
        job = mx_job_default();
    mx_handle_t xjob = MX_HANDLE_INVALID;
    mx_handle_duplicate(job, MX_RIGHT_SAME_RIGHTS, &xjob);
    return launchpad_create_with_jobs(job, xjob, name, result);
}

mx_handle_t launchpad_get_process_handle(launchpad_t* lp) {
    return lp_proc(lp);
}

mx_handle_t launchpad_get_root_vmar_handle(launchpad_t* lp) {
    return lp_vmar(lp);
}

mx_status_t launchpad_set_args(launchpad_t* lp,
                                int argc, const char* const* argv) {
    if (lp->error)
        return lp->error;
    if (argc < 0)
        return lp_error(lp, ERR_INVALID_ARGS, "arguments: negative argc");

    size_t total = 0;
    for (int i = 0; i < argc; ++i)
        total += strlen(argv[i]) + 1;

    char* buffer = NULL;
    if (total > 0) {
        buffer = malloc(total);
        if (buffer == NULL)
            return lp_error(lp, ERR_NO_MEMORY, "arguments: out of memory");

        char* p = buffer;
        for (int i = 0; i < argc; ++i)
            p = stpcpy(p, argv[i]) + 1;

        if ((size_t) (p - buffer) != total) {
            // The strings changed in parallel.  Not kosher!
            free(buffer);
            return lp_error(lp, ERR_INVALID_ARGS, "arguments: trickery");
        }
    }

    free(lp->args);
    lp->argc = argc;
    lp->args = buffer;
    lp->args_len = total;
    return NO_ERROR;
}

mx_status_t launchpad_set_environ(launchpad_t* lp, const char* const* envp) {
    if (lp->error)
        return lp->error;

    size_t total = 0;
    char* buffer = NULL;
    uint32_t envc = 0;

    if (envp != NULL) {
        for (const char* const* ep = envp; *ep != NULL; ++ep) {
            total += strlen(*ep) + 1;
            ++envc;
        }
    }

    if (total > 0) {
        buffer = malloc(total);
        if (buffer == NULL)
            return lp_error(lp, ERR_NO_MEMORY, "environ: out of memory");

        char* p = buffer;
        for (const char* const* ep = envp; *ep != NULL; ++ep)
            p = stpcpy(p, *ep) + 1;

        if ((size_t) (p - buffer) != total) {
            // The strings changed in parallel.  Not kosher!
            free(buffer);
            return lp_error(lp, ERR_INVALID_ARGS, "environ: trickery");
        }
    }

    free(lp->env);
    lp->envc = envc;
    lp->env = buffer;
    lp->env_len = total;
    return NO_ERROR;
}

static mx_status_t more_handles(launchpad_t* lp, size_t n) {
    if (lp->error)
        return lp->error;

    if (lp->handle_alloc - lp->handle_count < n) {
        size_t alloc = lp->handle_alloc == 0 ? 8 : lp->handle_alloc * 2;
        mx_handle_t* handles = realloc(lp->handles,
                                       alloc * sizeof(handles[0]));
        if (handles == NULL)
            return lp_error(lp, ERR_NO_MEMORY, "out of memory for handle table");
        lp->handles = handles;
        uint32_t* info = realloc(lp->handles_info, alloc * sizeof(info[0]));
        if (info == NULL)
            return lp_error(lp, ERR_NO_MEMORY, "out of memory for handle table");
        lp->handles_info = info;
        lp->handle_alloc = alloc;
    }
    return NO_ERROR;
}

mx_status_t launchpad_add_handle(launchpad_t* lp, mx_handle_t h, uint32_t id) {
    if (h == MX_HANDLE_INVALID)
        return lp_error(lp, ERR_BAD_HANDLE, "added invalid handle");
    mx_status_t status = more_handles(lp, 1);
    if (status == NO_ERROR) {
        lp->handles[lp->handle_count] = h;
        lp->handles_info[lp->handle_count] = id;
        ++lp->handle_count;
    } else {
        mx_handle_close(h);
    }
    return status;
}

mx_status_t launchpad_add_handles(launchpad_t* lp, size_t n,
                                  const mx_handle_t h[],
                                  const uint32_t id[]) {
    mx_status_t status = more_handles(lp, n);
    if (status == NO_ERROR) {
        memcpy(&lp->handles[lp->handle_count], h, n * sizeof(h[0]));
        memcpy(&lp->handles_info[lp->handle_count], id, n * sizeof(id[0]));
        lp->handle_count += n;
        for (size_t i = 0; i < n; i++) {
            if (h[i] == MX_HANDLE_INVALID) {
                return lp_error(lp, ERR_BAD_HANDLE, "added invalid handle");
            }
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            mx_handle_close(h[i]);
        }
    }
    return status;
}

//TODO: use transfer_fd here and eliminate mxio_pipe_half()
mx_status_t launchpad_add_pipe(launchpad_t* lp, int* fd_out, int target_fd) {
    mx_handle_t handle;
    uint32_t id;
    int fd;

    if (lp->error)
        return lp->error;
    if ((target_fd < 0) || (target_fd >= MAX_MXIO_FD)) {
        return lp_error(lp, ERR_INVALID_ARGS, "add_pipe: invalid target fd");
    }
    mx_status_t status;
    if ((status = mxio_pipe_half(&handle, &id)) < 0) {
        return lp_error(lp, status, "add_pipe: failed to create pipe");
    }
    fd = status;
    if ((status = launchpad_add_handle(lp, handle, MX_HND_INFO(MX_HND_INFO_TYPE(id), target_fd))) < 0) {
        close(fd);
        mx_handle_close(handle);
        return status;
    }
    *fd_out = fd;
    return NO_ERROR;
}

static void check_elf_stack_size(launchpad_t* lp, elf_load_info_t* elf) {
    size_t elf_stack_size = elf_load_get_stack_size(elf);
    if (elf_stack_size > 0)
        launchpad_set_stack_size(lp, elf_stack_size);
}

mx_status_t launchpad_elf_load_basic(launchpad_t* lp, mx_handle_t vmo) {
    if (vmo < 0)
        return lp_error(lp, vmo, "elf_load: negative vmo");
    if (vmo == MX_HANDLE_INVALID)
        return lp_error(lp, ERR_INVALID_ARGS, "elf_load: invalid vmo");
    if (lp->error)
        goto done;

    elf_load_info_t* elf;
    mx_status_t status;
    if ((status = elf_load_start(vmo, &elf)))
        lp_error(lp, status, "elf_load: elf_load_start() failed");
    if ((status = elf_load_finish(lp_vmar(lp), elf, vmo, &lp->base, &lp->entry)))
        lp_error(lp, status, "elf_load: elf_load_finish() failed");
    check_elf_stack_size(lp, elf);
    elf_load_destroy(elf);

    if (status == NO_ERROR) {
        lp->loader_message = false;
    }

done:
    mx_handle_close(vmo);
    return lp->error;
}

mx_status_t launchpad_elf_load_extra(launchpad_t* lp, mx_handle_t vmo,
                                     mx_vaddr_t* base, mx_vaddr_t* entry) {
    if (lp->error)
        return lp->error;
    if (vmo < 0)
        return lp_error(lp, vmo, "elf_load_extra: negative vmo");
    if (vmo == MX_HANDLE_INVALID)
        return lp_error(lp, ERR_INVALID_ARGS, "elf_load_extra: invalid vmo");

    elf_load_info_t* elf;
    mx_status_t status;
    if ((status = elf_load_start(vmo, &elf)))
        lp_error(lp, status, "elf_load_extra: elf_load_start() failed");
    if ((status = elf_load_finish(lp_vmar(lp), elf, vmo, base, entry)))
        lp_error(lp, status, "elf_load_extra: elf_load_finish() failed");
    elf_load_destroy(elf);

    return lp->error;
}

#define LOADER_SVC_MSG_MAX 1024

static mx_handle_t loader_svc_rpc(mx_handle_t loader_svc, uint32_t opcode,
                                  const void* data, size_t len) {
    static atomic_uint_fast32_t next_txid;

    struct {
        mx_loader_svc_msg_t header;
        uint8_t data[LOADER_SVC_MSG_MAX - sizeof(mx_loader_svc_msg_t)];
    } msg;

    if (len >= sizeof(msg.data))
        return ERR_BUFFER_TOO_SMALL;

    memset(&msg.header, 0, sizeof(msg.header));
    msg.header.txid = atomic_fetch_add(&next_txid, 1);
    msg.header.opcode = opcode;
    memcpy(msg.data, data, len);
    msg.data[len] = 0;

    mx_handle_t handle = MX_HANDLE_INVALID;
    const mx_channel_call_args_t call = {
        .wr_bytes = &msg,
        .wr_num_bytes = sizeof(msg.header) + len + 1,
        .rd_bytes = &msg,
        .rd_num_bytes = sizeof(msg),
        .rd_handles = &handle,
        .rd_num_handles = 1,
    };
    uint32_t reply_size;
    uint32_t handle_count;
    mx_status_t read_status = NO_ERROR;
    mx_status_t status = mx_channel_call(loader_svc, 0, MX_TIME_INFINITE,
                                         &call, &reply_size, &handle_count,
                                         &read_status);
    if (status != NO_ERROR) {
        return status == ERR_CALL_FAILED ? read_status : status;
    }

    // Check for protocol violations.
    if (reply_size != sizeof(msg.header)) {
    protocol_violation:
        mx_handle_close(handle);
        return ERR_BAD_STATE;
    }
    if (msg.header.opcode != LOADER_SVC_OP_STATUS)
        goto protocol_violation;

    if (msg.header.arg != NO_ERROR) {
        if (handle != MX_HANDLE_INVALID)
            goto protocol_violation;
        if (msg.header.arg > 0)
            goto protocol_violation;
        handle = msg.header.arg;
    }

    return handle;
}

static mx_status_t setup_loader_svc(launchpad_t* lp) {
    if (lp->special_handles[HND_LOADER_SVC] != MX_HANDLE_INVALID)
        return NO_ERROR;

    mx_handle_t loader_svc = mxio_loader_service(NULL, NULL);
    if (loader_svc < 0)
        return loader_svc;

    lp->special_handles[HND_LOADER_SVC] = loader_svc;
    return NO_ERROR;
}

// Consumes 'vmo' on success, not on failure.
static mx_status_t handle_interp(launchpad_t* lp, mx_handle_t vmo,
                                 const char* interp, size_t interp_len) {
    mx_status_t status = setup_loader_svc(lp);
    if (status != NO_ERROR)
        return status;

    mx_handle_t interp_vmo = loader_svc_rpc(
        lp->special_handles[HND_LOADER_SVC], LOADER_SVC_OP_LOAD_OBJECT,
        interp, interp_len);
    if (interp_vmo < 0)
        return interp_vmo;

    elf_load_info_t* elf;
    status = elf_load_start(interp_vmo, &elf);
    if (status == NO_ERROR) {
        status = elf_load_finish(lp_vmar(lp), elf, interp_vmo,
                                 &lp->base, &lp->entry);
        elf_load_destroy(elf);
    }
    mx_handle_close(interp_vmo);

    if (status == NO_ERROR) {
        if (lp->special_handles[HND_EXEC_VMO] != MX_HANDLE_INVALID)
            mx_handle_close(lp->special_handles[HND_EXEC_VMO]);
        lp->special_handles[HND_EXEC_VMO] = vmo;
        lp->loader_message = true;
    }

    return status;
}

mx_status_t launchpad_elf_load(launchpad_t* lp, mx_handle_t vmo) {
    if (vmo < 0)
        return lp_error(lp, vmo, "elf_load: negative vmo");
    if (vmo == MX_HANDLE_INVALID)
        return lp_error(lp, ERR_INVALID_ARGS, "elf_load: invalid vmo");
    if (lp->error)
        goto done;

    elf_load_info_t* elf;
    mx_status_t status;
    if ((status = elf_load_start(vmo, &elf)) != NO_ERROR) {
        lp_error(lp, status, "elf_load: elf_load_start() failed");
    } else {
        char* interp;
        size_t interp_len;
        status = elf_load_get_interp(elf, vmo, &interp, &interp_len);
        if (status != NO_ERROR) {
            lp_error(lp, status, "elf_load: get_interp() failed");
        } else {
            if (interp == NULL) {
                status = elf_load_finish(lp_vmar(lp), elf, vmo,
                                         &lp->base, &lp->entry);
                if (status != NO_ERROR) {
                    lp_error(lp, status, "elf_load: elf_load_finish() failed");
                } else {
                    lp->loader_message = false;
                }
            } else {
                if ((status = handle_interp(lp, vmo, interp, interp_len))) {
                    lp_error(lp, status, "elf_load: handle_interp failed");
                } else {
                    // handle_interp() takes ownership of vmo on success
                    vmo = MX_HANDLE_INVALID;
                }
                free(interp);
            }
            if (status == NO_ERROR)
                check_elf_stack_size(lp, elf);
        }
        elf_load_destroy(elf);
    }
done:
    if (vmo)
        mx_handle_close(vmo);
    return lp->error;
}

static mx_handle_t vdso_vmo = MX_HANDLE_INVALID;
static mtx_t vdso_mutex = MTX_INIT;
static void vdso_lock(void) {
    mtx_lock(&vdso_mutex);
}
static void vdso_unlock(void) {
    mtx_unlock(&vdso_mutex);
}
static mx_handle_t vdso_get_vmo(void) {
    if (vdso_vmo == MX_HANDLE_INVALID)
        vdso_vmo = mxio_get_startup_handle(
            MX_HND_INFO(MX_HND_TYPE_VDSO_VMO, 0));
    return vdso_vmo;
}

mx_handle_t launchpad_get_vdso_vmo(void) {
    vdso_lock();
    mx_handle_t result;
    mx_status_t status = mx_handle_duplicate(vdso_get_vmo(),
                                             MX_RIGHT_SAME_RIGHTS, &result);
    vdso_unlock();
    if (status < 0) {
        return status;
    } else {
        return result;
    }
}

mx_handle_t launchpad_set_vdso_vmo(mx_handle_t new_vdso_vmo) {
    vdso_lock();
    mx_handle_t old = vdso_vmo;
    vdso_vmo = new_vdso_vmo;
    vdso_unlock();
    return old;
}

mx_status_t launchpad_add_vdso_vmo(launchpad_t* lp) {
    if (lp->error)
        return lp->error;
    mx_handle_t vdso = launchpad_get_vdso_vmo();
    if (vdso < 0)
        return lp_error(lp, vdso, "add_vdso_vmo: get_vdso_vmo failed");
    mx_status_t status = launchpad_add_handle(
        lp, vdso, MX_HND_INFO(MX_HND_TYPE_VDSO_VMO, 0));
    if (status != NO_ERROR)
        mx_handle_close(vdso);
    return status;
}

mx_status_t launchpad_load_vdso(launchpad_t* lp, mx_handle_t vmo) {
    if (vmo != MX_HANDLE_INVALID)
        return launchpad_elf_load_extra(lp, vmo, &lp->vdso_base, NULL);
    vdso_lock();
    vmo = vdso_get_vmo();
    mx_status_t status = launchpad_elf_load_extra(lp, vmo,
                                                  &lp->vdso_base, NULL);
    vdso_unlock();
    return status;
}

mx_status_t launchpad_get_entry_address(launchpad_t* lp, mx_vaddr_t* entry) {
    if (lp->entry == 0)
        return ERR_BAD_STATE;
    *entry = lp->entry;
    return NO_ERROR;
}

mx_status_t launchpad_get_base_address(launchpad_t* lp, mx_vaddr_t* base) {
    if (lp->base == 0)
        return ERR_BAD_STATE;
    *base = lp->base;
    return NO_ERROR;
}

bool launchpad_send_loader_message(launchpad_t* lp, bool do_send) {
    bool result = lp->loader_message;
    if (!lp->error)
        lp->loader_message = do_send;
    return result;
}

mx_handle_t launchpad_use_loader_service(launchpad_t* lp, mx_handle_t svc) {
    if (lp->error) {
        mx_handle_close(svc);
        return lp->error;
    }
    mx_handle_t result = lp->special_handles[HND_LOADER_SVC];
    lp->special_handles[HND_LOADER_SVC] = svc;
    return result;
}

static mx_status_t send_loader_message(launchpad_t* lp,
                                       mx_handle_t first_thread,
                                       mx_handle_t tochannel) {
    struct loader_msg {
        mx_proc_args_t header;
        uint32_t handle_info[HND_SPECIAL_COUNT + HND_LOADER_COUNT];
        char args_and_env[];
    };

    const size_t msg_size =
        sizeof(struct loader_msg) + lp->args_len + lp->env_len;
    struct loader_msg* msg = malloc(msg_size);
    if (msg == NULL)
        return ERR_NO_MEMORY;

    memset(&msg->header, 0, sizeof(msg->header));
    msg->header.protocol = MX_PROCARGS_PROTOCOL;
    msg->header.version = MX_PROCARGS_VERSION;
    msg->header.handle_info_off = offsetof(struct loader_msg, handle_info);

    // Include the argument strings so the dynamic linker can use argv[0]
    // in messages it prints.
    if (lp->argc > 0) {
        msg->header.args_off = offsetof(struct loader_msg, args_and_env);
        msg->header.args_num = lp->argc;
        memcpy(msg->args_and_env, lp->args, lp->args_len);
    }

    // Include the environment strings so the dynamic linker can
    // see options like LD_DEBUG or whatnot.
    if (lp->envc > 0) {
        msg->header.environ_off
            = offsetof(struct loader_msg, args_and_env) + lp->args_len;
        msg->header.environ_num = lp->envc;
        memcpy(&msg->args_and_env[lp->args_len], lp->env, lp->env_len);
    }

    // This loop should be completely unrolled.  But using a switch here
    // gives us compiler warnings if we forget to handle any of the special
    // types listed in the enum.
    mx_handle_t handles[HND_SPECIAL_COUNT + HND_LOADER_COUNT];
    size_t nhandles = 0;
    for (enum special_handles i = 0; i <= HND_SPECIAL_COUNT; ++i) {
        uint32_t id = 0; // -Wall
        switch (i) {
        case HND_SPECIAL_COUNT:;
            // Duplicate the handles for the loader so we can send them in the
            // loader message and still have them later.
            mx_handle_t proc;
            mx_status_t status = mx_handle_duplicate(lp_proc(lp), MX_RIGHT_SAME_RIGHTS, &proc);
            if (status != NO_ERROR) {
                free(msg);
                return status;
            }
            mx_handle_t vmar;
            status = mx_handle_duplicate(lp_vmar(lp), MX_RIGHT_SAME_RIGHTS, &vmar);
            if (status != NO_ERROR) {
                mx_handle_close(proc);
                free(msg);
                return status;
            }
            mx_handle_t thread;
            status = mx_handle_duplicate(first_thread, MX_RIGHT_SAME_RIGHTS, &thread);
            if (status != NO_ERROR) {
                mx_handle_close(proc);
                mx_handle_close(vmar);
                free(msg);
                return status;
            }
            handles[nhandles] = proc;
            msg->handle_info[nhandles] = MX_HND_TYPE_PROC_SELF;
            handles[nhandles + 1] = vmar;
            msg->handle_info[nhandles + 1] = MX_HND_TYPE_VMAR_ROOT;
            handles[nhandles + 2] = thread;
            msg->handle_info[nhandles + 2] = MX_HND_TYPE_THREAD_SELF;
            nhandles += HND_LOADER_COUNT;
            continue;

        case HND_LOADER_SVC:
            id = MX_HND_TYPE_LOADER_SVC;
            break;

        case HND_EXEC_VMO:
            id = MX_HND_TYPE_EXEC_VMO;
            break;
        }
        if (lp->special_handles[i] != MX_HANDLE_INVALID) {
            handles[nhandles] = lp->special_handles[i];
            msg->handle_info[nhandles] = id;
            ++nhandles;
        }
    }

    mx_status_t status = mx_channel_write(tochannel, 0, msg, msg_size,
                                          handles, nhandles);
    if (status == NO_ERROR) {
        // message_write consumed all those handles.
        for (enum special_handles i = 0; i < HND_SPECIAL_COUNT; ++i)
            lp->special_handles[i] = MX_HANDLE_INVALID;
        lp->loader_message = false;
    } else {
        // Close the handles we duplicated for the loader.
        // The others remain live in the launchpad.
        for (int i = 1; i <= HND_LOADER_COUNT; i++) {
          mx_handle_close(handles[nhandles - i]);
        }
    }

    free(msg);
    return status;
}

// TODO(mcgrathr): One day we'll have a gather variant of message_write
// and then we can send this without copying into a temporary buffer.
static void* build_message(launchpad_t* lp, size_t *total_size) {
    size_t total = sizeof(mx_proc_args_t);
    total += lp->handle_count * sizeof(lp->handles_info[0]);
    total += lp->args_len;
    total += lp->env_len;

    uint8_t* buffer = malloc(total);
    if (buffer == NULL)
        return NULL;

    uint8_t* p = buffer;
    mx_proc_args_t* header = (void*)p;
    p += sizeof(*header);

    memset(header, 0, sizeof(*header));
    header->protocol = MX_PROCARGS_PROTOCOL;
    header->version = MX_PROCARGS_VERSION;

    header->handle_info_off = p - buffer;
    p = mempcpy(p, lp->handles_info,
                lp->handle_count * sizeof(lp->handles_info[0]));

    if (lp->argc > 0) {
        header->args_num = lp->argc;
        header->args_off = p - buffer;
        p = mempcpy(p, lp->args, lp->args_len);
    }

    if (lp->envc > 0) {
        header->environ_num = lp->envc;
        header->environ_off = p - buffer;
        p = mempcpy(p, lp->env, lp->env_len);
    }

    *total_size = total;
    return buffer;
}

size_t launchpad_set_stack_size(launchpad_t* lp, size_t new_size) {
    size_t old_size = lp->stack_size;
    if (new_size >= (SIZE_MAX & -PAGE_SIZE)) {
        // Ridiculously large size won't actually work at allocation time,
        // but at least page rounding won't wrap it around to zero.
        new_size = SIZE_MAX & -PAGE_SIZE;
    } else if (new_size > 0) {
        // Round up to page size.
        new_size = (new_size + PAGE_SIZE - 1) & -PAGE_SIZE;
    }
    if (lp->error == NO_ERROR)
        lp->stack_size = new_size;
    return old_size;
}

static mx_status_t prepare_start(launchpad_t* lp, const char* thread_name,
                                 mx_handle_t to_child,
                                 mx_handle_t* thread, uintptr_t* sp) {
    if (lp->entry == 0)
        return ERR_BAD_STATE;

    *sp = 0;
    if (lp->stack_size > 0) {
        // Allocate the initial thread's stack.
        mx_handle_t stack_vmo;
        mx_status_t status = mx_vmo_create(lp->stack_size, 0, &stack_vmo);
        if (status < 0)
            return lp_error(lp, status, "cannot create stack vmo");
        mx_vaddr_t stack_base;
        status = mx_vmar_map(lp_vmar(lp), 0, stack_vmo, 0, lp->stack_size,
                              MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE,
                              &stack_base);
        if (status == NO_ERROR) {
            DEBUG_ASSERT(lp->stack_size % PAGE_SIZE == 0);
            *sp = compute_initial_stack_pointer(stack_base, lp->stack_size);
            // Pass the stack VMO to the process.  Our protocol with the
            // new process is that we warrant that this is the VMO from
            // which the initial stack is mapped and that we've exactly
            // mapped the entire thing, so vm_object_get_size on this in
            // concert with the initial SP value tells it the exact bounds
            // of its stack.
            status = launchpad_add_handle(lp, stack_vmo,
                                          MX_HND_TYPE_STACK_VMO);
        }
        if (status != NO_ERROR) {
            mx_handle_close(stack_vmo);
            return lp_error(lp, status, "cannot map stack vmo");
        }
    }

    mx_status_t status = mx_thread_create(lp_proc(lp), thread_name,
                                          strlen(thread_name), 0, thread);
    if (status < 0) {
        return lp_error(lp, status, "cannot create initial thread");
    } else {
        // Pass the thread handle down to the child.  The handle we pass
        // will be consumed by message_write.  So we need a duplicate to
        // pass to mx_process_start later.
        mx_handle_t thread_copy;
        status = mx_handle_duplicate(*thread, MX_RIGHT_SAME_RIGHTS, &thread_copy);
        if (status < 0) {
            mx_handle_close(*thread);
            return lp_error(lp, status, "cannot duplicate thread handle");
        }
        status = launchpad_add_handle(lp, thread_copy,
                                      MX_HND_TYPE_THREAD_SELF);
        if (status != NO_ERROR) {
            mx_handle_close(*thread);
            return status;
        }
    }

    if (lp->loader_message) {
        status = send_loader_message(lp, *thread, to_child);
        if (status != NO_ERROR) {
            mx_handle_close(*thread);
            return lp_error(lp, status, "failed to send loader message");
        }
    }

    size_t size;
    void* msg = build_message(lp, &size);
    if (msg == NULL) {
        mx_handle_close(*thread);
        return lp_error(lp, ERR_NO_MEMORY, "out of memory assembling procargs message");
    }

    // Assume the process will read the bootstrap message onto its
    // initial thread's stack.  If it would need more than half its
    // stack just to read the message, consider that an unreasonably
    // large size for the message (presumably arguments and
    // environment strings that are unreasonably large).
    if (size > lp->stack_size / 2) {
        free(msg);
        mx_handle_close(*thread);
        return lp_error(lp, ERR_BUFFER_TOO_SMALL, "procargs message is too large");
    }

    status = mx_channel_write(to_child, 0, msg, size,
                              lp->handles, lp->handle_count);
    free(msg);
    if (status == NO_ERROR) {
        // message_write consumed all the handles.
        for (size_t i = 0; i < lp->handle_count; ++i)
            lp->handles[i] = MX_HANDLE_INVALID;
        lp->handle_count = 0;
    } else {
        mx_handle_close(*thread);
        return lp_error(lp, status, "failed to write procargs message");
    }

    return NO_ERROR;
}

mx_handle_t launchpad_start(launchpad_t* lp) {
    if (lp->error)
        return lp->error;

    // The proc handle in lp->handles[0] will be consumed by message_write.
    // So we'll need a duplicate to do process operations later.
    mx_handle_t proc;
    mx_status_t status = mx_handle_duplicate(lp_proc(lp), MX_RIGHT_SAME_RIGHTS, &proc);
    if (status < 0)
        return lp_error(lp, status, "start: cannot duplicate process handle");

    mx_handle_t channelh[2];
    status = mx_channel_create(0, channelh, channelh + 1);
    if (status != NO_ERROR) {
        mx_handle_close(proc);
        return lp_error(lp, status, "start: cannot create channel");
    }
    mx_handle_t to_child = channelh[0];
    mx_handle_t child_bootstrap = channelh[1];

    mx_handle_t thread;
    uintptr_t sp;
    status = prepare_start(lp, "main", to_child, &thread, &sp);
    mx_handle_close(to_child);
    if (status != NO_ERROR) {
        lp_error(lp, status, "start: prepare_start() failed");
    } else {
        status = mx_process_start(proc, thread, lp->entry, sp,
                                  child_bootstrap, lp->vdso_base);
        if (status != NO_ERROR)
            lp_error(lp, status, "start: mx_process_start() failed");
        mx_handle_close(thread);
    }
    // process_start consumed child_bootstrap if successful.
    if (status == NO_ERROR)
        return proc;

    mx_handle_close(child_bootstrap);
    return status;
}

mx_status_t launchpad_start_injected(launchpad_t* lp, const char* thread_name,
                                     mx_handle_t to_child,
                                     uintptr_t bootstrap_handle_in_child) {
    if (lp->error)
        return lp->error;

    mx_handle_t thread;
    uintptr_t sp;
    mx_status_t status = prepare_start(lp, thread_name, to_child,
                                       &thread, &sp);
    if (status != NO_ERROR) {
        lp_error(lp, status, "start_injected: prepare_start() failed");
    } else {
        status = mx_thread_start(thread, lp->entry, sp,
                                 bootstrap_handle_in_child, lp->vdso_base);
        if (status != NO_ERROR) {
            lp_error(lp, status, "start_injected: mx_thread_start() failed");
        }
        mx_handle_close(thread);
    }
    return status;
}

mx_status_t launchpad_go(launchpad_t* lp, mx_handle_t* proc, const char** errmsg) {
    mx_handle_t h = launchpad_start(lp);
    if (errmsg)
        *errmsg = lp->errmsg;
    if (h > 0) {
        if (proc) {
            *proc = h;
        } else {
            mx_handle_close(h);
        }
        h = NO_ERROR;
    }
    launchpad_destroy(lp);
    return h;
}

mx_status_t launchpad_arguments(launchpad_t* lp,
                                int argc, const char* const* argv) {
    return launchpad_set_args(lp, argc, argv);
}

mx_status_t launchpad_environ(launchpad_t* lp, const char* const* envp) {
    return launchpad_set_environ(lp, envp);
}



#include <launchpad/vmo.h>

static mx_status_t launchpad_elf_load_with_vdso(launchpad_t* lp, mx_handle_t vmo) {
    launchpad_elf_load(lp, vmo);
    launchpad_load_vdso(lp, MX_HANDLE_INVALID);
    return launchpad_add_vdso_vmo(lp);
}

mx_status_t launchpad_load_from_file(launchpad_t* lp, const char* path) {
    return launchpad_elf_load_with_vdso(lp, launchpad_vmo_from_file(path));
}

mx_status_t launchpad_load_from_fd(launchpad_t* lp, int fd) {
    return launchpad_elf_load_with_vdso(lp, launchpad_vmo_from_fd(fd));
}

mx_status_t launchpad_load_from_vmo(launchpad_t* lp, mx_handle_t vmo) {
    return launchpad_elf_load_with_vdso(lp, vmo);
}
