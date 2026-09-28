// Host qualification helper: libjpeg's recoverable warnings count as failure.
// Ordinary Pillow decoding suppresses warnings and can successfully return an
// image after a damaged entropy stream. This probe decodes every scanline.
// Build against matching libjpeg headers/library; run with one JPEG path.
// Exit 0=clean decode, 1=decoder warning/error, 2=usage/input limits.
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
struct error_state { struct jpeg_error_mgr pub; jmp_buf trap; int warnings; };
static void emit(j_common_ptr common, int level) {
  if (level >= 0) return;
  struct error_state* e = (struct error_state*)common->err;
  ++e->warnings;
  char message[JMSG_LENGTH_MAX]; (*common->err->format_message)(common, message);
  fprintf(stderr, "JPEG warning: %s\n", message);
}
static void fatal(j_common_ptr common) {
  char message[JMSG_LENGTH_MAX]; (*common->err->format_message)(common, message);
  fprintf(stderr, "JPEG fatal: %s\n", message);
  longjmp(((struct error_state*)common->err)->trap, 1);
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
  struct jpeg_decompress_struct d = {0}; struct error_state e = {0};
  d.err = jpeg_std_error(&e.pub); e.pub.emit_message = emit; e.pub.error_exit = fatal;
  if (setjmp(e.trap)) { jpeg_destroy_decompress(&d); fclose(f); return 1; }
  jpeg_create_decompress(&d); jpeg_stdio_src(&d, f);
  jpeg_read_header(&d, TRUE);
  if (!d.image_width || !d.image_height || d.image_width > 4096 || d.image_height > 4096) {
    jpeg_destroy_decompress(&d); fclose(f); return 2;
  }
  jpeg_start_decompress(&d);
  JSAMPARRAY row = (*d.mem->alloc_sarray)((j_common_ptr)&d, JPOOL_IMAGE, d.output_width*d.output_components, 1);
  while (d.output_scanline < d.output_height) jpeg_read_scanlines(&d, row, 1);
  jpeg_finish_decompress(&d);
  printf("%ux%u warnings=%d\n", d.output_width, d.output_height, e.warnings);
  jpeg_destroy_decompress(&d); fclose(f);
  return e.warnings ? 1 : 0;
}
