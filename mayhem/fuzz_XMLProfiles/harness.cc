// Mayhem wrapper around fuzz/C++/fuzz_XMLProfiles: the same load_XML_profiles_string() call, but each
// input starts from a clean library. The upstream harness leaves every parsed profile in process-global
// maps (emplace() then rejects a name that is already registered, so an input's result depended on the
// inputs before it) and lets an input's <log>/<library_settings> reconfigure the logger and library.
//
// An input's <log><consumer><class>FileConsumer</class> can also name any file for the library to
// create or truncate (FileConsumer opens it in its constructor). Every write-mode open() / fopen() the
// library makes (through libstdc++) is therefore redirected into one per-run scratch directory (the path
// flattened into a file name), and that directory is emptied after each input, so no input writes outside
// it or sees an earlier input's file.
#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/LibrarySettings.hpp>
#include <xmlparser/XMLProfileManager.h>

#include "fuzz_utils.hpp"

using namespace eprosima::fastdds;

static void clean_scratch_dir();

static const std::string& scratch_dir()
{
    // Never freed: the atexit handler below reads it, and a function-local static string would be
    // destroyed before that handler runs.
    static const std::string* const dir = new std::string([]()
            {
                const char* tmp = getenv("TMPDIR");
                std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/fuzz_XMLProfiles.XXXXXX";
                if (!mkdtemp(&tmpl[0]))
                {
                    abort();
                }
                atexit([]()
                {
                    clean_scratch_dir();
                    rmdir(scratch_dir().c_str());
                });
                return tmpl;
            }());
    return *dir;
}

static void clean_scratch_dir()
{
    DIR* d = opendir(scratch_dir().c_str());
    if (!d)
    {
        return;
    }
    while (dirent* e = readdir(d))
    {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
        {
            unlink((scratch_dir() + "/" + e->d_name).c_str());
        }
    }
    closedir(d);
}

static bool g_in_target = false;

// True for a call made by a shared object (libstdc++'s ofstream, which is how the library opens a log
// file) while an input is being processed. libFuzzer's own writes (crash artifacts), made from the
// executable itself, keep their path.
static bool redirect_wanted(
        void* caller)
{
    Dl_info self, from;
    return g_in_target && dladdr(reinterpret_cast<void*>(&clean_scratch_dir), &self) && dladdr(caller, &from) &&
           from.dli_fbase != self.dli_fbase;
}

// The scratch path standing in for `path`, or an empty string if the open must not be redirected.
static std::string scratch_path(
        const char* path)
{
    if (!path || !strncmp(path, "/dev/", 5) || !strncmp(path, "/proc/", 6))
    {
        return std::string();
    }
    std::string name(path);
    for (char& c : name)
    {
        if (c == '/')
        {
            c = '_';
        }
    }
    if (name.size() > 200)
    {
        name.resize(200);
    }
    return scratch_dir() + "/" + name;
}

static bool write_flags(
        int flags)
{
    return (flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)) || (flags & O_TMPFILE) == O_TMPFILE;
}

static FILE* open_file(
        const char* path,
        const char* mode,
        void* caller)
{
    static auto real = reinterpret_cast<FILE* (*)(const char*, const char*)>(dlsym(RTLD_NEXT, "fopen"));
    std::string redirected =
            strpbrk(mode, "wa+") && redirect_wanted(caller) ? scratch_path(path) : std::string();
    return real(redirected.empty() ? path : redirected.c_str(), mode);
}

static int open_fd(
        const char* path,
        int flags,
        mode_t mode,
        void* caller)
{
    static auto real = reinterpret_cast<int (*)(const char*, int, ...)>(dlsym(RTLD_NEXT, "open"));
    std::string redirected = write_flags(flags) && redirect_wanted(caller) ? scratch_path(path) : std::string();
    return real(redirected.empty() ? path : redirected.c_str(), flags, mode);
}

extern "C" FILE* fopen(
        const char* path,
        const char* mode)
{
    return open_file(path, mode, __builtin_return_address(0));
}

extern "C" FILE* fopen64(
        const char* path,
        const char* mode)
{
    return open_file(path, mode, __builtin_return_address(0));
}

#define OPEN_FUNCTION(name)                                         \
    extern "C" int name(const char* path, int flags, ...)           \
    {                                                               \
        mode_t mode = 0;                                            \
        if (flags & (O_CREAT | O_TMPFILE))                          \
        {                                                           \
            va_list ap;                                             \
            va_start(ap, flags);                                    \
            mode = va_arg(ap, mode_t);                              \
            va_end(ap);                                             \
        }                                                           \
        return open_fd(path, flags, mode, __builtin_return_address(0)); \
    }
OPEN_FUNCTION(open)
OPEN_FUNCTION(open64)

static void reset_global_state()
{
    xmlparser::XMLProfileManager::DeleteInstance();
    xmlparser::XMLProfileManager::library_settings(LibrarySettings());
    dds::Log::Reset();
    clean_scratch_dir();
}

extern "C" int LLVMFuzzerTestOneInput(
        const uint8_t* data,
        size_t size)
{
    static bool initialized = false;
    if (!initialized)
    {
        ignore_stdout();
        scratch_dir();
        initialized = true;
    }

    if (!size)
    {
        return -1;
    }

    g_in_target = true;
    dds::DomainParticipantFactory::get_instance()->load_XML_profiles_string(
        reinterpret_cast<const char*>(data), size);
    g_in_target = false;
    reset_global_state();

    return 0;
}
