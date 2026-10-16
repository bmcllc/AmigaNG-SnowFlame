/* sf_fs.h - acesso ao SnowFS do DVD pelo runtime (stdio usa isto; a camada de plataforma lista arquivos por aqui). */
#ifndef SF_FS_H
#define SF_FS_H
int sf_fs_mount(void);                  /* 0 = ok */
int sf_fs_count(void);
const char *sf_fs_name(int i);
unsigned sf_fs_size(int i);
int sf_fs_find(const char *path);       /* índice ou -1 */
int sf_ram_count(void);                 /* arquivos criados em RAM por fopen("w") */
const char *sf_ram_name(int i);
unsigned sf_ram_size(int i);
unsigned long long sf_time_ns(void);    /* tempo desde o boot, em ns */
#endif
