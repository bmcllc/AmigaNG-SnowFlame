/* huffgen.c - roda no HOST durante o build: monta a árvore inicial de Huffman do ioq3 (MSG_initHuffman: um Huff_addRef por unidade de
 * msg_hData, ~870 M instruções no ColdFire = ~6,6 s de boot) com o próprio huffman.c e grava o estado como índices em sf_huff.h.
 * No SF-100, MSG_initHuffman só restaura a tabela (patch de msg.c em apply_patches.sh): a árvore é a exata do Q3 (demos e servidores 1.32).
 * Compilação (build_ioq3.sh): cc -I code/qcommon -DSF_HDATA='"msg_hdata.h"' huffgen.c code/qcommon/huffman.c; msg_hdata.h é o vetor
 * msg_hData extraído de msg.c. Antes de gravar, confere que a tabela restaurada codifica todos os símbolos como a árvore original. */
#include "q_shared.h"
#include "qcommon.h"
#include <stdio.h>

static int msg_hData[256] = {
#include SF_HDATA
};

/* ponteiro -> inteiro: -1 nulo, 0..767 nó de nodeList, 1000+k entrada k de nodePtrs (a lista livre guarda ponteiros para nodePtrs) */
static int enc(huff_t *h, const void *p) {
  if (!p) return -1;
  if ((const node_t *)p >= h->nodeList && (const node_t *)p < h->nodeList + 768) return (int)((const node_t *)p - h->nodeList);
  if ((node_t *const *)p >= h->nodePtrs && (node_t *const *)p < h->nodePtrs + 768) return 1000 + (int)((node_t *const *)p - h->nodePtrs);
  fprintf(stderr, "huffgen: ponteiro fora do huff_t\n"); exit(1);
}
static void *dec(huff_t *h, int v) { return v < 0 ? NULL : v < 1000 ? (void *)&h->nodeList[v] : (void *)&h->nodePtrs[v - 1000]; }

static int tab[16384], nt;
static void put(int v) { if (nt == (int)(sizeof tab / sizeof tab[0])) { fprintf(stderr, "huffgen: tabela cheia\n"); exit(1); } tab[nt++] = v; }
static void save(huff_t *h) {
  int i; node_t *n;
  put(h->blocNode); put(h->blocPtrs); put(enc(h, h->tree)); put(enc(h, h->lhead)); put(enc(h, h->ltail)); put(enc(h, h->freelist));
  for (i = 0; i < HMAX + 1; i++) put(enc(h, h->loc[i]));
  for (i = 0; i < h->blocNode; i++) { n = &h->nodeList[i]; put(enc(h, n->left)); put(enc(h, n->right)); put(enc(h, n->parent)); put(enc(h, n->next)); put(enc(h, n->prev)); put(enc(h, n->head)); put(n->weight); put(n->symbol); }
  for (i = 0; i < h->blocPtrs; i++) put(enc(h, h->nodePtrs[i]));
}
/* mesmo código que vai no sf_huff.h (o restaurador do motor), usado aqui na verificação */
#define SF_HUFF_RESTORE_BODY \
  const int *t = sf_huff_tab; int i; node_t *n; \
  h->blocNode = *t++; h->blocPtrs = *t++; h->tree = dec(h, *t++); h->lhead = dec(h, *t++); h->ltail = dec(h, *t++); h->freelist = dec(h, *t++); \
  for (i = 0; i < HMAX + 1; i++) h->loc[i] = dec(h, *t++); \
  for (i = 0; i < h->blocNode; i++) { n = &h->nodeList[i]; n->left = dec(h, *t++); n->right = dec(h, *t++); n->parent = dec(h, *t++); n->next = dec(h, *t++); n->prev = dec(h, *t++); n->head = dec(h, *t++); n->weight = *t++; n->symbol = *t++; } \
  for (i = 0; i < h->blocPtrs; i++) h->nodePtrs[i] = dec(h, *t++);
static const int *sf_huff_tab = tab;
static void restore(huff_t *h) { SF_HUFF_RESTORE_BODY }
#define STR2(...) #__VA_ARGS__
#define STR(...) STR2(__VA_ARGS__)

int main(int argc, char **argv) {
  static huffman_t ref; static huff_t back; int i, j; byte a[64], b[64]; int oa, ob; FILE *f;
  if (argc != 2) { fprintf(stderr, "uso: huffgen sf_huff.h\n"); return 2; }
  Huff_Init(&ref);
  for (i = 0; i < 256; i++) for (j = 0; j < msg_hData[i]; j++) Huff_addRef(&ref.compressor, (byte)i);
  save(&ref.compressor);
  memset(&back, 0, sizeof back); restore(&back);
  for (i = 0; i < 256; i++) {   /* mesma sequência de bits para cada símbolo, e o mesmo símbolo de volta */
    memset(a, 0, sizeof a); memset(b, 0, sizeof b); oa = ob = 0;
    Huff_offsetTransmit(&ref.compressor, i, a, &oa, 512); Huff_offsetTransmit(&back, i, b, &ob, 512);
    if (oa != ob || memcmp(a, b, sizeof a)) { fprintf(stderr, "huffgen: tabela diverge no símbolo %d\n", i); return 1; }
    if (!ref.compressor.loc[i]) continue;   /* símbolo fora da árvore: vai como NYT + 8 bits */
    ob = 0; Huff_offsetReceive(back.tree, &j, b, &ob, 512); if (j != i) { fprintf(stderr, "huffgen: decodificação diverge no símbolo %d\n", i); return 1; }
  }
  if (!(f = fopen(argv[1], "w"))) { perror(argv[1]); return 1; }
  fprintf(f, "/* sf_huff.h - GERADO por port/ioq3/huffgen.c: árvore inicial de Huffman do Q3 (MSG_initHuffman) como índices. Não edite. */\n");
  fprintf(f, "static const int sf_huff_tab[%d] = {", nt);
  for (i = 0; i < nt; i++) fprintf(f, i == 0 ? "\n%d" : i % 24 ? ",%d" : ",\n%d", tab[i]);
  fprintf(f, "\n};\nstatic void *sf_huff_dec(huff_t *h, int v) { return v < 0 ? NULL : v < 1000 ? (void *)&h->nodeList[v] : (void *)&h->nodePtrs[v - 1000]; }\n");
  fprintf(f, "#define dec sf_huff_dec\nstatic void sf_huff_restore(huff_t *h) { %s }\n#undef dec\n", STR(SF_HUFF_RESTORE_BODY));
  fclose(f);
  printf("%s: %d valores\n", argv[1], nt);
  return 0;
}
