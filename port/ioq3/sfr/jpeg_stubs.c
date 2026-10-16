/* jpeg_stubs.c - o SF-100 só DECODIFICA JPEG; as rotinas de compressão (usadas só por screenshots em tr_image_jpg.c) viram no-ops. */
#include <stdio.h>
#include <stddef.h>
#include "jpeglib.h"
void jpeg_CreateCompress(j_compress_ptr cinfo, int version, size_t structsize) { (void)cinfo; (void)version; (void)structsize; }
void jpeg_set_defaults(j_compress_ptr cinfo) { (void)cinfo; }
void jpeg_set_quality(j_compress_ptr cinfo, int q, boolean f) { (void)cinfo; (void)q; (void)f; }
void jpeg_start_compress(j_compress_ptr cinfo, boolean w) { (void)cinfo; (void)w; }
JDIMENSION jpeg_write_scanlines(j_compress_ptr cinfo, JSAMPARRAY s, JDIMENSION n) { (void)cinfo; (void)s; (void)n; return 0; }
void jpeg_finish_compress(j_compress_ptr cinfo) { (void)cinfo; }
void jpeg_destroy_compress(j_compress_ptr cinfo) { (void)cinfo; }
