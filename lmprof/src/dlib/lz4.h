/*
** Local declarations for Defold's dmLZ4 API.
** Mirrors the small part of dlib/lz4.h needed by the Tracy exporter.
*/
#ifndef lmprof_dlib_lz4_h
#define lmprof_dlib_lz4_h

#include <stdint.h>

#define DMLZ4_MAX_OUTPUT_SIZE (1LL << 31)

namespace dmLZ4
{
    enum Result
    {
        RESULT_OK                    = 0,
        RESULT_COMPRESSION_FAILED    = 1,
        RESULT_OUTBUFFER_TOO_SMALL   = 2,
        RESULT_INPUT_SIZE_TOO_LARGE  = 3,
        RESULT_OUTPUT_SIZE_TOO_LARGE = 4,
    };

    Result DecompressBuffer(const void* buffer, uint32_t buffer_size, void* decompressed_buffer, uint32_t max_output, int* decompressed_size);
    Result CompressBuffer(const void* buffer, uint32_t buffer_size, void* compressed_buffer, int* compressed_size);
    Result MaxCompressedSize(int uncompressed_size, int* max_compressed_size);
}

#endif
