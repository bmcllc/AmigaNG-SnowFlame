/* triangle.c - exemplo do SnowOS Native: desenha um triângulo Gouraud com o FlameCore (SnowGX) e toca um tom (SnowAudio).
 *   build/snowcc os/apps/triangle.c -I os -I os/include --run */
#include "snowos_all.c"

unsigned cmd[64], verts[36]; short tone[256];

void app_main(void) {
  GxList l;
  gx_init(); snd_init();
  gx_put_vertex(&verts[0],  32 << 16,  8 << 16, 0x8000, 0xFFFF0000, 0, 0);
  gx_put_vertex(&verts[12], 56 << 16, 56 << 16, 0x8000, 0xFF00FF00, 0, 0);
  gx_put_vertex(&verts[24],  8 << 16, 56 << 16, 0x8000, 0xFF0000FF, 0, 0);
  os_cache_flush((unsigned)verts, sizeof verts);
  gx_begin(&l, cmd, 64);
  gx_set_rt(&l, VRAM_BASE, 256, 64, 64); gx_clear(&l, 0xFF101020, 0x3F800000, CLR_COLOR | CLR_DEPTH);
  gx_vertex_base(&l, (unsigned)verts); gx_state(&l, 0); gx_draw_arrays(&l, 0, 3, PRIM_TRIS); gx_end(&l);
  os_printf("GPU: %s\n", gx_submit(&l) == 0 ? "ok" : "erro");
  snd_sine(tone, 256, 64, 12000); snd_load(0, tone, sizeof tone); snd_voice_play(0, 0, 256, SND_PCM16, 0x10000, 256, 256, 1);
  task_sleep(20);
  os_printf("pixel central = %x\n", REG(VRAM_BASE + 40 * 256 + 32 * 4));
  os_exit(0);
}
