/* snowin.c - SnowIn: teclado (FIFO de eventos USB HID) e mouse */
#include "snowos.h"

static Sem in_sem;
static int in_ready;

static void in_setup(void) {
  if (in_ready) return; in_ready = 1;
  sem_init(&in_sem, 0); os_irq_attach(15, &in_sem); REG(HID_BASE + HID_CTRL) = 1;
}

int in_key_poll(void) { unsigned v; in_setup(); v = REG(HID_BASE + HID_KEY_DATA); return v == 0xFFFFFFFFu ? -1 : (int)v; }
int in_key_wait(void) {
  int ev;
  in_setup();
  for (;;) { ev = in_key_poll(); if (ev >= 0) return ev; sem_wait_timeout(&in_sem, 20); }
}

void in_mouse(int *dx, int *dy, int *wheel) {
  in_setup();
  if (dx) *dx = (int)REG(HID_BASE + HID_MOUSE_DX); else (void)REG(HID_BASE + HID_MOUSE_DX);
  if (dy) *dy = (int)REG(HID_BASE + HID_MOUSE_DY); else (void)REG(HID_BASE + HID_MOUSE_DY);
  if (wheel) *wheel = (int)REG(HID_BASE + HID_MOUSE_WHEEL); else (void)REG(HID_BASE + HID_MOUSE_WHEEL);
}
unsigned in_mouse_buttons(void) { in_setup(); return REG(HID_BASE + HID_MOUSE_BTN); }

int in_key_ascii(int ev) {
  static const char lo[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
  static const char hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";
  int code = ev & 0xFF, shift = (ev >> 16) & KEYM_SHIFT;
  if (ev & KEY_UP) return 0;
  if (code < 0x04 || code > 0x38) return 0;
  return shift ? hi[code - 4] : lo[code - 4];
}
