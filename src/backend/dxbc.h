/* dxbc.h: small edits to D3D shader bytecode (DXBC), for backends that load it.
 * Plain C, no platform headers, so it is tested headlessly. */
#ifndef BARE_DXBC_H
#define BARE_DXBC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The container's checksum (an MD5 with DXBC's own padding) over blob[20..]. */
void bare_dxbc_checksum(const unsigned char *blob, size_t size, uint32_t out[4]);

/* Clears the "refactoringAllowed" global flag in place and fixes the checksum.
 * With the flag, the driver may reassociate and fuse float maths as it likes,
 * and NVIDIA's does so differently depending on the code around it; without
 * it, every instruction rounds as written, which is how browsers' WebGL
 * shaders come out. Returns false if the blob isn't DXBC it understands. */
bool bare_dxbc_make_strict(unsigned char *blob, size_t size);

#endif
