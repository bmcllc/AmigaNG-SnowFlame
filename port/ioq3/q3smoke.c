/* q3smoke.c - roda código REAL do ioquake3 (q_shared.c, q_math.c) no ColdFire do SF-100 e imprime resultados que o host reproduz com o mesmo fonte. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "q_shared.h"

void Com_Error(int level, const char *fmt, ...) { (void)level; printf("Com_Error: %s\n", fmt); }
void Com_Printf(const char *fmt, ...) { (void)fmt; }

int main(void) {
  vec3_t a = {1.5f, -2.0f, 3.25f}, n, fwd, right, up, ang = {20.0f, 135.0f, 10.0f}, perp, d;
  vec3_t mins = {-1, -1, -1}, maxs = {2, 3, 4};
  float len; char buf[MAX_INFO_STRING]; char tok[64]; char *p; vec3_t axis[3], axis2[3], prod[3]; cplane_t pl;
  VectorCopy(a, n); len = VectorNormalize(n);
  printf("normalize: len=%.4f n=(%.4f %.4f %.4f)\n", len, n[0], n[1], n[2]);
  AngleVectors(ang, fwd, right, up);
  printf("angles: f=(%.4f %.4f %.4f) r=(%.4f %.4f %.4f) u=(%.4f %.4f %.4f)\n", fwd[0], fwd[1], fwd[2], right[0], right[1], right[2], up[0], up[1], up[2]);
  PerpendicularVector(perp, fwd); printf("perp: %.4f %.4f %.4f dot=%.5f\n", perp[0], perp[1], perp[2], DotProduct(perp, fwd));
  AnglesToAxis(ang, axis); AxisCopy(axis, axis2); MatrixMultiply(axis, axis2, prod);
  printf("matmul: %.4f %.4f %.4f / %.4f %.4f %.4f\n", prod[0][0], prod[0][1], prod[0][2], prod[2][0], prod[2][1], prod[2][2]);
  printf("vectoangles: "); vectoangles(fwd, d); printf("%.3f %.3f %.3f\n", d[0], d[1], d[2]);
  printf("anglenorm: %.3f %.3f %.3f\n", AngleNormalize360(-725.5f), AngleNormalize180(725.5f), AngleSubtract(10, 350));
  printf("boxontoplane: "); VectorSet(pl.normal, 0.267261f, 0.534522f, 0.801784f); pl.dist = 1.5f; SetPlaneSignbits(&pl); pl.type = 3;
  printf("%d signbits=%d\n", BoxOnPlaneSide(mins, maxs, &pl), pl.signbits);
  printf("rotate: "); { vec3_t src = {1, 0, 0}, dir = {0, 0, 1}, dst; RotatePointAroundVector(dst, dir, src, 90); printf("%.4f %.4f %.4f\n", dst[0], dst[1], dst[2]); }
  printf("stricmp: %d %d %d  strncmp: %d  clean: ", Q_stricmp("HeLLo", "hello"), Q_stricmp("abc", "abd") < 0, Q_stricmpn("Quake3", "quAKE4", 5), Q_strncmp("abc", "abd", 2));
  { char t[32]; Q_strncpyz(t, "  Hello^1World^7!  ", sizeof t); Q_CleanStr(t); printf("[%s] len=%d\n", t, Q_PrintStrlen("^1ab^2cd")); }
  buf[0] = 0; Info_SetValueForKey(buf, "name", "Visor"); Info_SetValueForKey(buf, "skill", "3"); Info_SetValueForKey(buf, "model", "sarge/blue");
  printf("info: %s |", buf); printf(" name=%s", Info_ValueForKey(buf, "name")); printf(" skill=%s", Info_ValueForKey(buf, "skill")); printf(" model=%s\n", Info_ValueForKey(buf, "model"));
  { static char text[] = "map q3dm17 \"quoted token\" 3.5e2 // comment\nnext"; p = text; }
  while (1) { const char *t = COM_Parse(&p); if (!t[0]) break; Q_strncpyz(tok, t, sizeof tok); printf("tok<%s> ", tok); } printf("\n");
  printf("va: %s", va("%d-%s-%5.2f", 42, "x", 3.14159)); printf(" %s\n", va("0x%08x", 0xBEEF));
  { char s[64]; Com_sprintf(s, sizeof s, "%s_%i_%c", "frag", 7, 'Z'); printf("sprintf: %s  atof=%.3f atoi=%d\n", s, atof("-12.375"), atoi("0042")); }
  printf("sqrt: %.5f %.5f  floor: %d  shortswap: %04x longswap: %08x\n", sqrt(1234.5), (double)Q_rsqrt(2.0f), (int)floor(-0.5), (unsigned)ShortSwap(0x1234) & 0xFFFF, (unsigned)LongSwap(0x11223344));
  { float x = 0.1f, sum = 0; int i; for (i = 0; i < 10; i++) sum += x; printf("float sum: %.7f  (int)%d  rint: %d %d\n", sum, (int)(sum * 1000), (int)lrintf(2.5f), (int)lrintf(-2.5f)); }
  return 0;
}
