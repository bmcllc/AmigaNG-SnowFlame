/* SDL_opengl.h - substituto mínimo: o renderer do SF-100 não usa OpenGL; só os tipos que os cabeçalhos comuns do ioq3 mencionam. */
#ifndef SF_SDL_OPENGL_H
#define SF_SDL_OPENGL_H
typedef unsigned int GLenum, GLuint, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat, GLclampf;
typedef double GLdouble, GLclampd;
typedef unsigned char GLboolean, GLubyte;
typedef void GLvoid;
typedef long GLsizeiptr, GLintptr;
typedef char GLchar;
typedef unsigned short GLushort;
typedef short GLshort;
typedef signed char GLbyte;
#define APIENTRY
#define APIENTRYP *
#endif
