#ifndef SHADY_RUNTIME_H
#define SHADY_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool use_validation;
    bool dump_spv;
    bool allow_no_devices;
} RuntimeConfig;

RuntimeConfig shd_rt_default_config();
void shd_rt_cli_parse_runtime_config(RuntimeConfig* config, int* pargc, char** argv);

typedef struct Runtime_  Runtime;
typedef struct Device_   Device;
typedef struct Program_  Program;
typedef struct Command_  Command;
typedef struct Buffer_   Buffer;

Runtime* shd_rt_initialize(RuntimeConfig config);
void shd_rt_shutdown(Runtime* runtime);

size_t shd_rt_device_count(Runtime* r);
Device* shd_rt_get_device(Runtime* r, size_t i);
Device* shd_rt_get_an_device(Runtime* r);
const char* shd_rt_get_device_name(Device* d);

typedef struct CompilerConfig_ CompilerConfig;
typedef struct Module_ Module;

Program* shd_rt_new_program_from_module(Runtime* runtime, const CompilerConfig* base_config, Module* mod);

typedef struct {
    uint64_t* profiled_gpu_time;
} ExtraKernelOptions;

Command* shd_rt_launch_kernel(Program* p, Device* d, const char* entry_point, int dimx, int dimy, int dimz, int args_count, void** args, ExtraKernelOptions* extra_options);
bool shd_rt_wait_completion(Command* cmd);

Buffer* shd_rt_allocate_buffer_device(Device* device, size_t bytes);
bool shd_rt_can_import_host_memory(Device* device);
Buffer* shd_rt_import_buffer_host(Device* device, void* ptr, size_t bytes);
void shd_rt_destroy_buffer(Buffer* buf);

void* shd_rt_get_buffer_host_pointer(Buffer* buf);
uint64_t shd_rt_get_buffer_device_pointer(Buffer* buf);

bool shd_rt_copy_to_buffer(Buffer* dst, size_t buffer_offset, void* src, size_t size);
bool shd_rt_copy_from_buffer(Buffer* src, size_t buffer_offset, void* dst, size_t size);

#endif
