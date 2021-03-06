#ifndef SL2_DR_CLIENT_H
#define SL2_DR_CLIENT_H

#include <winsock2.h>
#include <winhttp.h>
#include <Windows.h>
#include <Winternl.h>
#include <Rpc.h>
#include <io.h>
#include <Dbghelp.h>
#include <psapi.h>

#include "vendor/json.hpp"
#include "dr_api.h"
#include "drmgr.h"
#include "drwrap.h"
#include "droption.h"
#include "drreg.h"

#include <string>

using namespace std;

extern "C" {
#include "util.h"
#include "uuid.h"
}

#include "common/sl2_dr_allocator.hpp"

/** Used for iterating over the function-module pair table. */
#define SL2_FUNCMOD_TABLE_SIZE (sizeof(SL2_FUNCMOD_TABLE) / sizeof(SL2_FUNCMOD_TABLE[0]))

/** Used for debugging prints. */
#define SL2_DR_DEBUG(...) (dr_fprintf(STDERR, __VA_ARGS__))

/**
 NOTE(ww): This loop is here because dr_fprintf has an internal buffer
 of 2048, and our JSON objects frequently exceed that. When that happens,
 dr_fprintf silently truncates them and confuses the harness with invalid JSON.
 We circumvent this by chunking the output.
 */
#define SL2_LOG_JSONL(json)                                                                        \
  do {                                                                                             \
    auto jsonl_str = json.dump();                                                                  \
    for (int i = 0; i < jsonl_str.length(); i += 1024) {                                           \
      dr_fprintf(STDERR, "%s", jsonl_str.substr(i, 1024).c_str());                                 \
    }                                                                                              \
    dr_fprintf(STDERR, "\n");                                                                      \
  } while (0)

/** Bits of the address that won't get randomized by ASLR */
#define SUB_ASLR_BITS 0xffff

// Macros for the function prototypes passed to pre- and post-function hooks.
// TODO(ww): There should really only be one of each of these (pre and post), but
// Microsoft's C preprocessor isn't C99 compliant and so makes things with __VA_ARGS__ hard.

/** Macros for quickly building a map of function names to function pre-hooks. */
#define SL2_PRE_HOOK1(map, func) (map[#func] = wrap_pre_##func)
/** Macros for quickly building a map of function names to function pre-hooks. */
#define SL2_PRE_HOOK2(map, func, hook_func) (map[#func] = wrap_pre_##hook_func)

/** Macros for quickly building a map of function names to function post-hooks. */
#define SL2_POST_HOOK1(map, func) (map[#func] = wrap_post_##func)
/** Macros for quickly building a map of function names to function post-hooks. */
#define SL2_POST_HOOK2(map, func, hook_func) (map[#func] = wrap_post_##hook_func)

/** The set of currently supported functions.*/
enum class Function {
  ReadFile,
  recv,
  WinHttpReadData,
  InternetReadFile,
  WinHttpWebSocketReceive,
  RegQueryValueEx,
  ReadEventLog,
  fread,
  fread_s,
  _read,
  MapViewOfFile,
};

/** The set of supported function targeting techniques. */
enum {
  /*! Target a function by its index, e.g. the 5th `fread` call */
  MATCH_INDEX = 1 << 0,
  /*! Target a function by its address, e.g. the `fread` at address 0x0000000a */
  MATCH_RETN_ADDRESS = 1 << 1,
  /*! Target a function by a hash calculated from its arguments */
  MATCH_ARG_HASH = 1 << 2,
  /*! Target a function by contents of argument buffer */
  MATCH_ARG_COMPARE = 1 << 3,
  /*! Target a single file across multiple reads */
  LOW_PRECISION = 1 << 4,
  /*! Target a single buffer across multiple reads */
  MEDIUM_PRECISION = 1 << 5,
  /*! Target a single read from a single buffer */
  HIGH_PRECISION = 1 << 6,
  /*! Target a byte-for-byte filename */
  MATCH_FILENAMES = 1 << 7,
  /*! Target call counts by return address */
  MATCH_RETN_COUNT = 1 << 8,

};

/** The struct filled with function information for hashing. See `MATCH_ARG_HASH`.
 * Note that the member names aren't quite right for all the function calls we fill this struct out
 * for. We mix and match as necessary since it's just used as a contiguous region of memory for
 * hashing, not an actual record.
 */
struct hash_context {
  wchar_t fileName[MAX_PATH + 1]; /*! Name of the file (if applicable) */
  size_t count;
  size_t position;
  size_t readSize;
};

/**
 * The struct filled with targeting information for a function.
 */
typedef struct targetFunction {
  /*! Whether or not the user selected this function in the GUI */
  bool selected;
  /*! The number of times we've encountered this function during execution */
  uint64_t index;
  /*! Which targeting strategy to use for this function */
  uint64_t mode;
  /*! The ASLR-independent return address (relative to the module) */
  uint64_t retAddrOffset;
  /*! The number of times we've encountered this return address during execution*/
  uint64_t retAddrCount;
  /*! The name of the function */
  string functionName;
  /*! The hash of the arguments of the function */
  string argHash;
  /*! The name of the source file (if available) */
  wstring source;
  /*! The first few bytes of the argument buffer*/
  vector<uint8_t> buffer;
} TargetFunction;

/**
 * Information for read in fuzzer and tracer clients
 */
struct client_read_info {
  /*! The place within the buffer we're reading from */
  size_t position;
  /*! The return address (releative to the module) of this function */
  size_t retAddrOffset;
  /*! Which function we've hooked */
  Function function;
  /*! Handle to the target */
  HANDLE hFile;
  /*! Number of bytes this function actually read */
  DWORD *lpNumberOfBytesRead;
  /*! Pointer to the buffer containing the user bytes (ie - the file being read) */
  void *lpBuffer;
  /*! Pointer to the hex-encoded hash of the arguments */ // TODO(ww): Make this a wchar_t * for
                                                          // consistency.
  char *argHash;
  /*! Pointer to the string name of the source file (if applicable) */
  wchar_t *source;
  /*! Number of bytes this function wants to read */
  size_t nNumberOfBytesToRead;
};

/**
 * The struct filled with exception information for registering within a minidump.
 */
struct sl2_exception_ctx {
  DWORD thread_id;
  EXCEPTION_RECORD record;
  CONTEXT thread_ctx;
};

/**
 * Represents a tuple of a function and its expected module.
 */
struct sl2_funcmod {
  const char *func;
  const char *mod;
};

// Typedefs for common types.
typedef void (*sl2_pre_proto)(void *, void **);
typedef void (*sl2_post_proto)(void *, void *);

typedef std::map<char *, sl2_pre_proto, std::less<char *>,
                 sl2_dr_allocator<std::pair<const char *, sl2_pre_proto>>>
    sl2_pre_proto_map;
typedef std::map<char *, sl2_post_proto, std::less<char *>,
                 sl2_dr_allocator<std::pair<const char *, sl2_post_proto>>>
    sl2_post_proto_map;

typedef std::map<Function, uint64_t, std::less<Function>,
                 sl2_dr_allocator<std::pair<Function, uint64_t>>>
    sl2_call_counts_map;

typedef std::map<uint64_t, uint64_t, std::less<uint64_t>,
                 sl2_dr_allocator<std::pair<uint64_t, uint64_t>>>
    sl2_retaddr_counts_map;

typedef nlohmann::basic_json<std::map, std::vector, std::string, bool, int64_t, uint64_t, double,
                             sl2_dr_allocator>
    json;

// Declared in sl2_dr_client.cpp; contains pairs of functions and their expected modules.
extern sl2_funcmod SL2_FUNCMOD_TABLE[];

///////////////////////////////////////////////////////////////////////////////////////////////////
// SL2Client
///////////////////////////////////////////////////////////////////////////////////////////////////
class SL2Client {

public:
  SL2Client();

  ////////////////////////////////////////////////////////////////////////////////////////////
  // Variables
  // TODO(ww): Subsume sl2_conn under SL2Client.
  /*! Map holding the number of times we've seen each function */
  sl2_call_counts_map call_counts;
  /*! Map holding the number of times we've seen each return address */
  sl2_retaddr_counts_map ret_addr_counts;
  /*! JSON object holding targeted functions */
  json parsedJson;
  /*! Base address for the main module */
  uint64_t baseAddr;

  ////////////////////////////////////////////////////////////////////////////////////////////
  // Methods
  // Method targeting methods.
  void hash_args(char *argHash, hash_context *fStruct);
  bool is_function_targeted(client_read_info *info);
  bool compare_filenames(targetFunction &t, client_read_info *info);
  bool compare_indices(targetFunction &t, Function &function);
  bool compare_index_at_retaddr(targetFunction &t, client_read_info *info);
  bool compare_return_addresses(targetFunction &t, client_read_info *info);
  bool compare_arg_hashes(targetFunction &t, client_read_info *info);
  bool compare_arg_buffers(targetFunction &t, client_read_info *info);
  bool function_is_in_expected_module(const char *func, const char *mod);

  // Crash-diversion mitigation methods.
  void wrap_pre_IsProcessorFeaturePresent(void *wrapcxt, OUT void **user_data);
  void wrap_post_IsProcessorFeaturePresent(void *wrapcxt, OUT void *user_data);
  void wrap_pre_UnhandledExceptionFilter(void *wrapcxt, OUT void **user_data,
                                         bool (*on_exception)(void *, dr_exception_t *));
  void wrap_pre_VerifierStopMessage(void *wrapcxt, OUT void **user_data,
                                    bool (*on_exception)(void *, dr_exception_t *));

  // Pre- and post-hook related methods.
  void wrap_pre_ReadEventLog(void *wrapcxt, OUT void **user_data);
  void wrap_pre_RegQueryValueEx(void *wrapcxt, OUT void **user_data);
  void wrap_pre_WinHttpWebSocketReceive(void *wrapcxt, OUT void **user_data);
  void wrap_pre_InternetReadFile(void *wrapcxt, OUT void **user_data);
  void wrap_pre_WinHttpReadData(void *wrapcxt, OUT void **user_data);
  void wrap_pre_recv(void *wrapcxt, OUT void **user_data);
  void wrap_pre_ReadFile(void *wrapcxt, OUT void **user_data);
  void wrap_pre_fread(void *wrapcxt, OUT void **user_data);
  void wrap_pre_fread_s(void *wrapcxt, OUT void **user_data);
  void wrap_pre__read(void *wrapcxt, OUT void **user_data);
  void wrap_pre_MapViewOfFile(void *wrapcxt, OUT void **user_data);
  bool is_sane_post_hook(void *wrapcxt, void *user_data, void **drcontext);
  bool loadTargets(string json);
  uint64_t increment_call_count(Function function);
  uint64_t increment_retaddr_count(uint64_t retAddr);

  // Utility methods.
  const char *function_to_string(Function function);
  const char *exception_to_string(DWORD exception_code);
};

/** Converts a JSON object into a `targetFunction`. */
SL2_EXPORT
void from_json(const json &j, targetFunction &t);

#endif
