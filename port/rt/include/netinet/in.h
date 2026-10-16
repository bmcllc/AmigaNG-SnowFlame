#ifndef SF_NETINET_IN_H
#define SF_NETINET_IN_H
/* o SF-100 não tem rede: só os tipos que o código do ioq3 referencia */
struct in_addr { unsigned s_addr; };
#define htons(x) ((unsigned short)(x))
#define ntohs(x) ((unsigned short)(x))
#define htonl(x) ((unsigned)(x))
#define ntohl(x) ((unsigned)(x))
#endif
