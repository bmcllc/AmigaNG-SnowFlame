/* SDL.h - substituto mínimo (sem SDL no SF-100): bibliotecas dinâmicas não existem; os módulos de jogo são ligados estaticamente. */
#ifndef SF_SDL_H
#define SF_SDL_H
void *SDL_LoadObject(const char *f);
void SDL_UnloadObject(void *h);
void *SDL_LoadFunction(void *h, const char *fn);
const char *SDL_GetError(void);
#endif
