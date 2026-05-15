#include <zlib.h>

#include "platform.h"
#include "thunk_gen.h"
#include "so_util.h"

// Direct passthroughs to the host libz. Unity 2020+ IL2CPP imports zlib for
// asset-bundle decompression (LZ4 + DEFLATE paths) and calls zlibVersion()
// during il2cpp_init — missing zlibVersion was the abort path on Hollow Knight.

DynLibFunction symtable_zlib[] = {
    THUNK_DIRECT(zlibVersion),

    THUNK_DIRECT(inflate),
    THUNK_DIRECT(inflateInit_),
    THUNK_DIRECT(inflateInit2_),
    THUNK_DIRECT(inflateEnd),
    THUNK_DIRECT(inflateReset),
    THUNK_DIRECT(inflateSync),
    THUNK_DIRECT(inflateCopy),
    THUNK_DIRECT(inflateSetDictionary),

    THUNK_DIRECT(deflate),
    THUNK_DIRECT(deflateInit_),
    THUNK_DIRECT(deflateInit2_),
    THUNK_DIRECT(deflateEnd),
    THUNK_DIRECT(deflateReset),
    THUNK_DIRECT(deflateCopy),
    THUNK_DIRECT(deflateSetDictionary),
    THUNK_DIRECT(deflateParams),
    THUNK_DIRECT(deflateBound),

    THUNK_DIRECT(compress),
    THUNK_DIRECT(compress2),
    THUNK_DIRECT(compressBound),
    THUNK_DIRECT(uncompress),

    THUNK_DIRECT(crc32),
    THUNK_DIRECT(crc32_combine),
    THUNK_DIRECT(adler32),
    THUNK_DIRECT(adler32_combine),

    THUNK_DIRECT(zError),
    THUNK_DIRECT(get_crc_table),

    {NULL, (uintptr_t)NULL}
};
