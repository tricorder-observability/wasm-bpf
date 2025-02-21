#include <asm/unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>

#include "bpf-api.h"

using namespace std;
extern "C" {
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
extern bool wasm_runtime_call_indirect(wasm_exec_env_t exec_env,
                                       uint32_t element_indices, uint32_t argc,
                                       uint32_t argv[]);
}
static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
                           va_list args) {
    if (DEBUG_LIBBPF_RUNTIME) return vfprintf(stderr, format, args);
    return 0;
}
void init_libbpf(void) {
    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);
    libbpf_set_print(libbpf_print_fn);
}

#define PERF_BUFFER_PAGES 64

typedef int (*bpf_buffer_sample_fn)(void *ctx, void *data, size_t size);
/// An absraction of a bpf ring buffer or perf buffer from bcc.
/// https://github.com/iovisor/bcc/blob/master/libbpf-tools/compat.c
struct bpf_buffer {
    struct bpf_map *events;
    void *inner;
    bpf_buffer_sample_fn fn;
    wasm_exec_env_t exec_env;
    uint32_t ctx;
    uint32_t wasm_sample_function;
    int type;
};

static int bpf_buffer_sample(void *ctx, void *data, size_t size) {
    wasm_bpf_program *program = (wasm_bpf_program *)ctx;
    size_t sample_size = size;
    if (program->max_poll_size < size) {
        sample_size = program->max_poll_size;
    }
    memcpy(program->poll_data, data, sample_size);
    wasm_module_inst_t module_inst =
        wasm_runtime_get_module_inst(program->buffer->exec_env);
    uint32_t argv[] = {
        program->buffer->ctx,
        wasm_runtime_addr_native_to_app(module_inst, program->poll_data),
        (uint32_t)size};
    auto buffer = program->buffer.get();
    if (!wasm_runtime_call_indirect(buffer->exec_env,
                                    buffer->wasm_sample_function, 3, argv)) {
        printf("call func1 failed\n");
        return 0xDEAD;
    }
    return 0;
}

static void perfbuf_sample_fn(void *ctx, int cpu, void *data, __u32 size) {
    bpf_buffer_sample(ctx, data, size);
}

struct bpf_map *bpf_obj_get_map_by_fd(int fd, bpf_object *obj) {
    bpf_map *map;
    bpf_object__for_each_map(map, obj) {
        if (bpf_map__fd(map) == fd) return map;
    }
    return NULL;
}

struct bpf_buffer *bpf_buffer__new(struct bpf_map *events) {
    struct bpf_buffer *buffer;
    bool use_ringbuf;
    int type;
    use_ringbuf = bpf_map__type(events) == BPF_MAP_TYPE_RINGBUF;
    if (use_ringbuf) {
        bpf_map__set_autocreate(events, false);
        type = BPF_MAP_TYPE_RINGBUF;
    } else {
        bpf_map__set_type(events, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
        bpf_map__set_key_size(events, sizeof(int));
        bpf_map__set_value_size(events, sizeof(int));
        type = BPF_MAP_TYPE_PERF_EVENT_ARRAY;
    }
    buffer = (bpf_buffer *)calloc(1, sizeof(*buffer));
    if (!buffer) {
        errno = ENOMEM;
        return NULL;
    }
    buffer->events = events;
    buffer->type = type;
    return buffer;
}

int bpf_buffer__open(struct bpf_buffer *buffer, bpf_buffer_sample_fn sample_cb,
                     void *ctx) {
    int fd, type;
    void *inner;

    fd = bpf_map__fd(buffer->events);
    type = buffer->type;

    switch (type) {
        case BPF_MAP_TYPE_PERF_EVENT_ARRAY:
            buffer->fn = sample_cb;
            inner = perf_buffer__new(fd, PERF_BUFFER_PAGES, perfbuf_sample_fn,
                                     NULL, ctx, NULL);
            break;
        case BPF_MAP_TYPE_RINGBUF:
            inner = ring_buffer__new(fd, sample_cb, ctx, NULL);
            break;
        default:
            return 0;
    }

    if (!inner) return -errno;

    buffer->inner = inner;
    return 0;
}

int bpf_buffer__poll(struct bpf_buffer *buffer, int timeout_ms) {
    switch (buffer->type) {
        case BPF_MAP_TYPE_PERF_EVENT_ARRAY:
            return perf_buffer__poll((perf_buffer *)buffer->inner, timeout_ms);
        case BPF_MAP_TYPE_RINGBUF:
            return ring_buffer__poll((ring_buffer *)buffer->inner, timeout_ms);
        default:
            return -EINVAL;
    }
}

void bpf_buffer__free(struct bpf_buffer *buffer) {
    if (!buffer) return;

    switch (buffer->type) {
        case BPF_MAP_TYPE_PERF_EVENT_ARRAY:
            perf_buffer__free((perf_buffer *)buffer->inner);
            break;
        case BPF_MAP_TYPE_RINGBUF:
            ring_buffer__free((ring_buffer *)buffer->inner);
            break;
    }
    free(buffer);
}
/// Get the file descriptor of a map by name.
int wasm_bpf_program::bpf_map_fd_by_name(const char *name) {
    return bpf_object__find_map_fd_by_name(obj.get(), name);
}
/// @brief load all bpf programs and maps in a object file.
int wasm_bpf_program::load_bpf_object(const void *obj_buf, size_t obj_buf_sz) {
    auto object = bpf_object__open_mem(obj_buf, obj_buf_sz, NULL);
    if (!object) {
        return (int)libbpf_get_error(object);
    }
    obj.reset(object);
    return bpf_object__load(object);
}

static int attach_cgroup(struct bpf_program *prog, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        printf("Failed to open cgroup\n");
        return -1;
    }
    if (!bpf_program__attach_cgroup(prog, fd)) {
        printf("Prog %s failed to attach cgroup %s\n", bpf_program__name(prog),
               path);
        return -1;
    }
    close(fd);
    return 0;
}

/// @brief attach a specific bpf program by name and target.
int wasm_bpf_program::attach_bpf_program(const char *name,
                                         const char *attach_target) {
    struct bpf_link *link;
    if (!attach_target) {
        // auto attach
        link = bpf_program__attach(
            bpf_object__find_program_by_name(obj.get(), name));
    } else {
        struct bpf_object *o = obj.get();
        struct bpf_program *prog = bpf_object__find_program_by_name(o, name);
        if (!prog) {
            printf("get prog %s fail", name);
            return -1;
        }
        const char *sec_name = bpf_program__section_name(prog);
        // TODO: support more attach type
        if (strcmp(sec_name, "sockops") == 0) {
            return attach_cgroup(prog, attach_target);
        } else {
            // try auto attach if new attach target is not supported
            link = bpf_program__attach(
                bpf_object__find_program_by_name(obj.get(), name));
        }
    }
    if (!link) {
        return (int)libbpf_get_error(link);
    }
    links.emplace(std::unique_ptr<bpf_link, int (*)(bpf_link * obj)>{
        link, bpf_link__destroy});
    return 0;
}

/// polling the buffer, if the buffer is not created, create it.
int wasm_bpf_program::bpf_buffer_poll(wasm_exec_env_t exec_env, int fd,
                                      int32_t sample_func, uint32_t ctx,
                                      void *data, size_t max_size,
                                      int timeout_ms) {
    if (buffer.get() == nullptr) {
        // create buffer
        auto map = bpf_obj_get_map_by_fd(fd, obj.get());
        buffer.reset(bpf_buffer__new(map));
        bpf_buffer__open(buffer.get(), bpf_buffer_sample, this);
        return 0;
    }
    max_poll_size = max_size;
    poll_data = data;
    buffer->exec_env = exec_env;
    buffer->wasm_sample_function = (uint32_t)sample_func;
    buffer->ctx = ctx;

    // poll the buffer
    int res = bpf_buffer__poll(buffer.get(), timeout_ms);
    if (res < 0) {
        return res;
    }
    return 0;
}

/// a wrapper function to call the bpf syscall
int bpf_map_operate(int fd, int cmd, void *key, void *value, void *next_key,
                    uint64_t flags) {
    switch (cmd) {
        case BPF_MAP_GET_NEXT_KEY:
            return bpf_map_get_next_key(fd, key, next_key);
        case BPF_MAP_LOOKUP_ELEM:
            return bpf_map_lookup_elem_flags(fd, key, value, flags);
        case BPF_MAP_UPDATE_ELEM:
            return bpf_map_update_elem(fd, key, value, flags);
        case BPF_MAP_DELETE_ELEM:
            return bpf_map_delete_elem_flags(fd, key, flags);
        default:  // More syscall commands can be allowed here
            return -EINVAL;
    }
    return -EINVAL;
}

extern "C" {
uint64_t wasm_load_bpf_object(wasm_exec_env_t exec_env, void *obj_buf,
                              int obj_buf_sz) {
    if (obj_buf_sz <= 0) return 0;
    wasm_bpf_program *program = new wasm_bpf_program();
    int res = program->load_bpf_object(obj_buf, (size_t)obj_buf_sz);
    if (res < 0) {
        delete program;
        return 0;
    }
    return (uint64_t)program;
}

int wasm_close_bpf_object(wasm_exec_env_t exec_env, uint64_t program) {
    delete ((wasm_bpf_program *)program);
    return 0;
}

int wasm_attach_bpf_program(wasm_exec_env_t exec_env, uint64_t program,
                            char *name, char *attach_target) {
    return ((wasm_bpf_program *)program)
        ->attach_bpf_program(name, attach_target);
}

int wasm_bpf_buffer_poll(wasm_exec_env_t exec_env, uint64_t program, int fd,
                         int32_t sample_func, uint32_t ctx, char *data,
                         int max_size, int timeout_ms) {
    return ((wasm_bpf_program *)program)
        ->bpf_buffer_poll(exec_env, fd, sample_func, ctx, data,
                          (size_t)max_size, timeout_ms);
}

int wasm_bpf_map_fd_by_name(wasm_exec_env_t exec_env, uint64_t program,
                            const char *name) {
    return ((wasm_bpf_program *)program)->bpf_map_fd_by_name(name);
}

int wasm_bpf_map_operate(wasm_exec_env_t exec_env, int fd, int cmd, void *key,
                         void *value, void *next_key, uint64_t flags) {
    return bpf_map_operate(fd, (bpf_map_cmd)cmd, key, value, next_key, flags);
}
}

int wasm_main(unsigned char *buf, unsigned int size, int argc, char *argv[]) {
    char error_buf[128];
    int exit_code = 0;
    char *wasm_path = NULL;
    wasm_module_t module = NULL;
    wasm_module_inst_t module_inst = NULL;
    wasm_exec_env_t exec_env = NULL;
    uint32_t stack_size = 1 << 20, heap_size = 1 << 20;
    wasm_function_inst_t start_func = NULL;
    uint32_t wasm_buffer = 0;
    RuntimeInitArgs init_args;

    memset(&init_args, 0, sizeof(RuntimeInitArgs));
    init_libbpf();
    // init wasm host functions
    static NativeSymbol native_symbols[] = {
        EXPORT_WASM_API_WITH_SIG(wasm_load_bpf_object, "(*~)I"),
        EXPORT_WASM_API_WITH_SIG(wasm_attach_bpf_program, "(I$$)i"),
        EXPORT_WASM_API_WITH_SIG(wasm_bpf_buffer_poll, "(Iiii*~i)i"),
        EXPORT_WASM_API_WITH_SIG(wasm_bpf_map_fd_by_name, "(I$)i"),
        EXPORT_WASM_API_WITH_SIG(wasm_bpf_map_operate, "(ii***I)i"),
        EXPORT_WASM_API_WITH_SIG(wasm_close_bpf_object, "(I)i"),
    };
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    init_args.n_native_symbols = sizeof(native_symbols) / sizeof(NativeSymbol);
    init_args.native_module_name = "wasm_bpf";
    init_args.native_symbols = native_symbols;
    // init runtime and wasi
    if (!wasm_runtime_full_init(&init_args)) {
        printf("Init runtime environment failed.\n");
        return -1;
    }
    module = wasm_runtime_load(buf, size, error_buf, sizeof(error_buf));
    if (!module) {
        printf("Load wasm module failed. error: %s\n", error_buf);
        return -1;
    }
    wasm_runtime_set_wasi_args(module, NULL, 0, NULL, 0, NULL, 0, argv, argc);
    module_inst = wasm_runtime_instantiate(module, stack_size, heap_size,
                                           error_buf, sizeof(error_buf));
    if (!module_inst) {
        printf("Instantiate wasm module failed. error: %s\n", error_buf);
        return -1;
    }
    exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
    if (!exec_env) {
        printf("Create wasm execution environment failed.\n");
        return -1;
    }
    wasm_runtime_set_module_inst(exec_env, module_inst);
    if (!(start_func = wasm_runtime_lookup_wasi_start_function(module_inst))) {
        printf("The start wasm function is not found.\n");
        return -1;
    }
    if (!wasm_runtime_call_wasm(exec_env, start_func, 0, NULL)) {
        printf("Call wasm function start failed. %s\n",
               wasm_runtime_get_exception(module_inst));
        return -1;
    }
    exit_code = wasm_runtime_get_wasi_exit_code(module_inst);
    if (exec_env) wasm_runtime_destroy_exec_env(exec_env);
    if (module_inst) {
        if (wasm_buffer) {
            wasm_runtime_module_free(module_inst, wasm_buffer);
        }
        wasm_runtime_deinstantiate(module_inst);
    }
    if (module) wasm_runtime_unload(module);
    wasm_runtime_destroy();
    return exit_code;
}
