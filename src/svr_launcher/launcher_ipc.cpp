#include "launcher_priv.h"

// The structure that will be located in the started process.
// It is used as a parameter to the below function.
struct IpcStructure
{
    // Windows API functions.
    decltype(LoadLibraryA)* LoadLibraryA;
    decltype(GetProcAddress)* GetProcAddress;
    decltype(SetDllDirectoryA)* SetDllDirectoryA;

    u32 unused;

    char library_name[256]; // The path to the library to load.
    char export_name[256]; // The export function to call.
    char svr_path[256]; // The path of the SVR directory. Does not end with a slash.
};

// The code that will run in the started process.
// See below for the actual code for these generated bytes.
#ifdef _WIN64
const u8 IPC_REMOTE_FUNC_BYTES[] =
{
    0x48, 0x89, 0x5c, 0x24, 0x08,
    0x57,
    0x48, 0x83, 0xec, 0x30,
    0x48, 0x8b, 0xf9,
    0x48, 0x8d, 0x99, 0x1c, 0x02,
    0x00, 0x00,
    0x48, 0x8b, 0xcb,
    0xff, 0x57, 0x10,
    0x48, 0x8b, 0x07,
    0x48, 0x8d, 0x4f, 0x1c,
    0xff, 0xd0,
    0x4c, 0x8b, 0x47, 0x08,
    0x48, 0x8d, 0x97, 0x1c, 0x01,
    0x00, 0x00,
    0x48, 0x8b, 0xc8,
    0x41, 0xff, 0xd0,
    0x8b, 0x4f, 0x18,
    0x89, 0x4c, 0x24, 0x28,
    0x48, 0x8d, 0x4c, 0x24, 0x20,
    0x48, 0x89, 0x5c, 0x24, 0x20,
    0xff, 0xd0,
    0x33, 0xc9,
    0xff, 0x57, 0x10,
    0x48, 0x8b, 0x5c, 0x24, 0x40,
    0x48, 0x83, 0xc4, 0x30,
    0x5f,
    0xc3,
};
#else
const u8 IPC_REMOTE_FUNC_BYTES[] =
{
    0x55,
    0x8b, 0xec,
    0x83, 0xec, 0x0c,
    0x56,
    0x57,
    0x8b, 0x7d, 0x08,
    0x8b, 0x47, 0x08,
    0x8d, 0xb7, 0x10, 0x02, 0x00,
    0x00,
    0x56,
    0xff, 0xd0,
    0x8b, 0x0f,
    0x8d, 0x47, 0x10,
    0x50,
    0xff, 0xd1,
    0x8b, 0x57, 0x04,
    0x8d, 0x8f, 0x10, 0x01, 0x00,
    0x00,
    0x51,
    0x50,
    0xff, 0xd2,
    0x8b, 0x4f, 0x0c,
    0x89, 0x4d, 0xf8,
    0x8d, 0x4d, 0xf4,
    0x51,
    0x89, 0x75, 0xf4,
    0xff, 0xd0,
    0x8b, 0x47, 0x08,
    0x83, 0xc4, 0x04,
    0x6a, 0x00,
    0xff, 0xd0,
    0x5f,
    0x5e,
    0x8b, 0xe5,
    0x5d,
    0xc2, 0x04, 0x00,
};
#endif

// You can use code listing in Visual Studio to generate the machine code for these functions:
// Using the property pages UI for unity_launcher.cpp, go to C/C++ -> Output Files -> Assembler Output and set it to Assembly With Machine Code (/FaC).
// Then compile the file and find the .cod file in the build directory. Open the file, find this function, and extract the bytes.
// For the function to actually be compiled in, it must be referenced. Enable the generation code in main for this and build in Release.
// Note that MSVC sometimes doesn't always create this file or it is sometimes not updated properly. Rebuild the project until it works.

// This is the function that will be injected into the target process.
// The instructions IPC_REMOTE_FUNC_BYTES above is the result of this function.
VOID CALLBACK ipc_remote_func(ULONG_PTR param)
{
    IpcStructure* data = (IpcStructure*)param;

    // There is no error handling here as there's no practical way to report
    // stuff back within this limited environment.
    // There have not been cases of these api functions failing with proper input
    // so let's stick with the simplest working solution for now.

    // Add our resource path as searchable to resolve library dependencies.
    data->SetDllDirectoryA(data->svr_path);

    // We need to call the right export in svr_standalone.dll.

    HMODULE module = data->LoadLibraryA(data->library_name);
    SvrGameInitFuncType init_func = (SvrGameInitFuncType)data->GetProcAddress(module, data->export_name);

    SvrGameInitData init_data;
    init_data.svr_path = data->svr_path;
    init_data.unused = 0;

    init_func(&init_data);

    // Restore the default search order.
    data->SetDllDirectoryA(NULL);
}

void LauncherState::ipc_setup_in_remote_process(LauncherGame* game, HANDLE process, HANDLE thread)
{
    // Allocate a sufficient enough size in the target process.
    // It needs to be able to contain all function bytes and the structure containing variable length strings.
    // The virtual memory that we allocated should not be freed as it will be used
    // as reference for future use within the application itself.
    void* remote_mem = VirtualAllocEx(process, NULL, 2048, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (remote_mem == NULL)
    {
        DWORD code = GetLastError();
        TerminateProcess(process, 1);
        svr_log("VirtualAllocEx failed with code %lu\n", code);
        launcher_error("Could not initialize standalone SVR. If you use an antivirus, add exception or disable.");
    }

    SIZE_T written = 0;
    u8* write_ptr = (u8*)remote_mem;

    void* remote_func_addr = write_ptr;
    WriteProcessMemory(process, write_ptr, IPC_REMOTE_FUNC_BYTES, sizeof(IPC_REMOTE_FUNC_BYTES), &written);
    write_ptr += written;

    // All addresses here must match up in the context of the target process, not our own.
    // The operating system api functions will always be located in the same address of every
    // process so those do not have to be adjusted.
    IpcStructure structure;
    structure.LoadLibraryA = LoadLibraryA;
    structure.GetProcAddress = GetProcAddress;
    structure.SetDllDirectoryA = SetDllDirectoryA;
    structure.unused = 0;

#ifdef _WIN64
    SVR_COPY_STRING(svr_va("%s\\svr_standalone64.dll", working_dir), structure.library_name);
#else
    SVR_COPY_STRING(svr_va("%s\\svr_standalone.dll", working_dir), structure.library_name);
#endif

    SVR_COPY_STRING("svr_init_from_launcher", structure.export_name);
    SVR_COPY_STRING(working_dir, structure.svr_path);

    void* remote_structure_addr = write_ptr;
    WriteProcessMemory(process, write_ptr, &structure, sizeof(structure), &written);
    write_ptr += written;

    // Queue up our procedural function to run instantly on the main thread when the process is resumed.
    if (!QueueUserAPC((PAPCFUNC)remote_func_addr, thread, (ULONG_PTR)remote_structure_addr))
    {
        DWORD code = GetLastError();
        TerminateProcess(process, 1);
        svr_log("QueueUserAPC failed with code %lu\n", code);
        launcher_error("Could not initialize standalone SVR. If you use an antivirus, add exception or disable.");
    }
}

void ipc_generate_bytes()
{
    IpcStructure structure = {};
    structure.LoadLibraryA = LoadLibraryA;
    structure.GetProcAddress = GetProcAddress;
    structure.SetDllDirectoryA = SetDllDirectoryA;

    // It is important to use QueueUserAPC here to produce the correct output.
    // Calling remote_func directly will produce uniquely optimized code which cannot
    // work in another process.
    QueueUserAPC(ipc_remote_func, GetCurrentThread(), (ULONG_PTR)&structure);

    // Used to signal the thread so the queued function will run.
    SleepEx(0, TRUE);
}
