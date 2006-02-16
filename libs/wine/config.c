/*
 * Configuration parameters shared between Wine server and clients
 *
 * Copyright 2002 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include "wine/library.h"

static const char server_config_dir[] = "/.wine";        /* config dir relative to $HOME */
static const char server_root_prefix[] = "/tmp/.wine-";  /* prefix for server root dir */
static const char server_dir_prefix[] = "/server-";      /* prefix for server dir */

static char *config_dir;
static char *server_dir;
static char *user_name;
static char *argv0_path;
static char *argv0_name;

#ifdef __GNUC__
static void fatal_error( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
static void fatal_perror( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
#endif

/* die on a fatal error */
static void fatal_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    va_end( args );
    exit(1);
}

/* die on a fatal error */
static void fatal_perror( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    perror( " " );
    va_end( args );
    exit(1);
}

/* malloc wrapper */
static void *xmalloc( size_t size )
{
    void *res;

    if (!size) size = 1;
    if (!(res = malloc( size ))) fatal_error( "virtual memory exhausted\n");
    return res;
}

/* strdup wrapper */
static char *xstrdup( const char *str )
{
    size_t len = strlen(str) + 1;
    char *res = xmalloc( len );
    memcpy( res, str, len );
    return res;
}

/* remove all trailing slashes from a path name */
inline static void remove_trailing_slashes( char *path )
{
    int len = strlen( path );
    while (len > 1 && path[len-1] == '/') path[--len] = 0;
}

/* determine where the destination path is located relative to the 'from' path */
inline static const char *get_relative_path( const char *from, const char *dest, unsigned int *dotdots )
{
#define DIR_END(p)  (*(p) == 0 || *(p) == '/')
    const char *start;

    *dotdots = 0;
    for (;;)
    {
        while (*from == '/') from++;
        while (*dest == '/') dest++;
        start = dest;  /* save start of next path element */
        if (!*from) break;

        while (!DIR_END(from) && *from == *dest) { from++; dest++; }
        if (DIR_END(from) && DIR_END(dest)) continue;

        /* count remaining elements in 'from' */
        do
        {
            (*dotdots)++;
            while (!DIR_END(from)) from++;
            while (*from == '/') from++;
        }
        while (*from);
        break;
    }
    return start;
#undef DIR_END
}

/* return the directory that contains the library at run-time */
static const char *get_runtime_libdir(void)
{
    static char *libdir;

#ifdef HAVE_DLADDR
    Dl_info info;

    if (!libdir && dladdr( get_runtime_libdir, &info ) && info.dli_fname[0] == '/')
    {
        const char *p = strrchr( info.dli_fname, '/' );
        unsigned int len = p - info.dli_fname;
        if (!len) len++;  /* include initial slash */
        libdir = xmalloc( len + 1 );
        memcpy( libdir, info.dli_fname, len );
        libdir[len] = 0;
    }
#endif /* HAVE_DLADDR */
    return libdir;
}

/* determine the proper location of the given path based on the current libdir */
static char *get_path_from_libdir( const char *path, const char *fallback, const char *filename )
{
    char *p, *ret;
    const char *libdir = get_runtime_libdir();

    /* retrieve the library load path */

    if (libdir)
    {
        unsigned int dotdots = 0;
        const char *start = get_relative_path( LIBDIR, path, &dotdots );

        ret = xmalloc( strlen(libdir) + 3 * dotdots + strlen(start) + strlen(filename) + 3 );
        strcpy( ret, libdir );
        p = ret + strlen(libdir);
        if (p[-1] != '/') *p++ = '/';

        while (dotdots--)
        {
            p[0] = '.';
            p[1] = '.';
            p[2] = '/';
            p += 3;
        }

        strcpy( p, start );
        p += strlen(p);
    }
    else
    {
        if (fallback) path = fallback;
        ret = xmalloc( strlen(path) + strlen(filename) + 2 );
        strcpy( ret, path );
        p = ret + strlen(ret);
    }

    if (*filename)
    {
        if (p[-1] != '/') *p++ = '/';
        strcpy( p, filename );
    }
    else if (p[-1] == '/') p[-1] = 0;
    return ret;
}

/* initialize the server directory value */
static void init_server_dir( dev_t dev, ino_t ino )
{
    char *p;
#ifdef HAVE_GETUID
    const unsigned int uid = getuid();
#else
    const unsigned int uid = 0;
#endif

    server_dir = xmalloc( sizeof(server_root_prefix) + 32 + sizeof(server_dir_prefix) +
                          2*sizeof(dev) + 2*sizeof(ino) );
    sprintf( server_dir, "%s%u%s", server_root_prefix, uid, server_dir_prefix );
    p = server_dir + strlen(server_dir);

    if (sizeof(dev) > sizeof(unsigned long) && dev > ~0UL)
        p += sprintf( p, "%lx%08lx-", (unsigned long)(dev >> 32), (unsigned long)dev );
    else
        p += sprintf( p, "%lx-", (unsigned long)dev );

    if (sizeof(ino) > sizeof(unsigned long) && ino > ~0UL)
        sprintf( p, "%lx%08lx", (unsigned long)(ino >> 32), (unsigned long)ino );
    else
        sprintf( p, "%lx", (unsigned long)ino );
}

/* retrieve the default dll dir */
const char *get_default_dlldir(void)
{
    static const char *dlldir;

    if (!dlldir) dlldir = get_path_from_libdir( DLLDIR, NULL, "" );
    return dlldir;
}

/* initialize all the paths values */
static void init_paths(void)
{
    struct stat st;

    const char *home = getenv( "HOME" );
    const char *user = NULL;
    const char *prefix = getenv( "WINEPREFIX" );

#ifdef HAVE_GETPWUID
    char uid_str[32];
    struct passwd *pwd = getpwuid( getuid() );

    if (pwd)
    {
        user = pwd->pw_name;
        if (!home) home = pwd->pw_dir;
    }
    if (!user)
    {
        sprintf( uid_str, "%u", getuid() );
        user = uid_str;
    }
#else  /* HAVE_GETPWUID */
    if (!(user = getenv( "USER" )))
        fatal_error( "cannot determine your user name, set the USER environment variable\n" );
#endif  /* HAVE_GETPWUID */
    user_name = xstrdup( user );

    /* build config_dir */

    if (prefix)
    {
        if (!(config_dir = strdup( prefix ))) fatal_error( "virtual memory exhausted\n");
        remove_trailing_slashes( config_dir );
        if (config_dir[0] != '/')
            fatal_error( "invalid directory %s in WINEPREFIX: not an absolute path\n", prefix );
        if (stat( config_dir, &st ) == -1)
        {
            if (errno != ENOENT)
                fatal_perror( "cannot open %s as specified in WINEPREFIX", config_dir );
            fatal_error( "the '%s' directory specified in WINEPREFIX doesn't exist.\n"
                         "You may want to create it by running 'wineprefixcreate'.\n", config_dir );
        }
    }
    else
    {
        if (!home) fatal_error( "could not determine your home directory\n" );
        if (home[0] != '/') fatal_error( "your home directory %s is not an absolute path\n", home );
        config_dir = xmalloc( strlen(home) + sizeof(server_config_dir) );
        strcpy( config_dir, home );
        remove_trailing_slashes( config_dir );
        strcat( config_dir, server_config_dir );
        if (stat( config_dir, &st ) == -1)
        {
            if (errno == ENOENT) return;  /* will be created later on */
            fatal_perror( "cannot open %s", config_dir );
        }
    }
    if (!S_ISDIR(st.st_mode)) fatal_error( "%s is not a directory\n", config_dir );

    init_server_dir( st.st_dev, st.st_ino );
}

/* initialize the argv0 path */
void wine_init_argv0_path( const char *argv0 )
{
    size_t size, len;
    const char *p;
    char *cwd;

    if (!(p = strrchr( argv0, '/' )))
    {
        argv0_name = xstrdup( argv0 );
        return;  /* if argv0 doesn't contain a path, don't store any path */
    }
    else argv0_name = xstrdup( p + 1 );

    len = p - argv0 + 1;
    if (argv0[0] == '/')  /* absolute path */
    {
        argv0_path = xmalloc( len + 1 );
        memcpy( argv0_path, argv0, len );
        argv0_path[len] = 0;
        return;
    }

    /* relative path, make it absolute */
    for (size = 256 + len; ; size *= 2)
    {
        if (!(cwd = malloc( size ))) break;
        if (getcwd( cwd, size - len ))
        {
            argv0_path = cwd;
            cwd += strlen(cwd);
            *cwd++ = '/';
            memcpy( cwd, argv0, len );
            cwd[len] = 0;
            return;
        }
        free( cwd );
        if (errno != ERANGE) break;
    }
}

/* return the configuration directory ($WINEPREFIX or $HOME/.wine) */
const char *wine_get_config_dir(void)
{
    if (!config_dir) init_paths();
    return config_dir;
}

/* return the full name of the server directory (the one containing the socket) */
const char *wine_get_server_dir(void)
{
    if (!server_dir)
    {
        if (!config_dir) init_paths();
        else
        {
            struct stat st;

            if (stat( config_dir, &st ) == -1)
            {
                if (errno != ENOENT) fatal_error( "cannot stat %s\n", config_dir );
                return NULL;  /* will have to try again once config_dir has been created */
            }
            init_server_dir( st.st_dev, st.st_ino );
        }
    }
    return server_dir;
}

/* return the current user name */
const char *wine_get_user_name(void)
{
    if (!user_name) init_paths();
    return user_name;
}

/* exec a binary using the preloader if requested; helper for wine_exec_wine_binary */
static void preloader_exec( char **argv, int use_preloader )
{
#ifdef linux
    if (use_preloader)
    {
        static const char preloader[] = "wine-preloader";
        char *p, *full_name;
        char **last_arg = argv, **new_argv;

        if (!(p = strrchr( argv[0], '/' ))) p = argv[0];
        else p++;

        full_name = xmalloc( p - argv[0] + sizeof(preloader) );
        memcpy( full_name, argv[0], p - argv[0] );
        memcpy( full_name + (p - argv[0]), preloader, sizeof(preloader) );

        /* make a copy of argv */
        while (*last_arg) last_arg++;
        new_argv = xmalloc( (last_arg - argv + 2) * sizeof(*argv) );
        memcpy( new_argv + 1, argv, (last_arg - argv + 1) * sizeof(*argv) );
        new_argv[0] = full_name;
        execv( full_name, new_argv );
        free( new_argv );
        free( full_name );
        return;
    }
#endif
    execv( argv[0], argv );
}

/* exec a wine internal binary (either the wine loader or the wine server) */
void wine_exec_wine_binary( const char *name, char **argv, const char *env_var )
{
    static const char bindir[] = BINDIR;
    const char *path, *pos, *ptr;
    int use_preloader = 0;

    if (!name)  /* no name means default loader */
    {
        name = argv0_name;
        use_preloader = 1;
    }

    /* first, bin directory from the current libdir or argv0 */
    argv[0] = get_path_from_libdir( bindir, argv0_path, name );
    preloader_exec( argv, use_preloader );
    free( argv[0] );

    /* then specified environment variable */
    if (env_var)
    {
        argv[0] = (char *)env_var;
        preloader_exec( argv, use_preloader );
    }

    /* now search in the Unix path */
    if ((path = getenv( "PATH" )))
    {
        argv[0] = xmalloc( strlen(path) + strlen(name) + 2 );
        pos = path;
        for (;;)
        {
            while (*pos == ':') pos++;
            if (!*pos) break;
            if (!(ptr = strchr( pos, ':' ))) ptr = pos + strlen(pos);
            memcpy( argv[0], pos, ptr - pos );
            strcpy( argv[0] + (ptr - pos), "/" );
            strcat( argv[0] + (ptr - pos), name );
            preloader_exec( argv, use_preloader );
            pos = ptr;
        }
        free( argv[0] );
    }

    /* and finally try BINDIR */
    argv[0] = xmalloc( sizeof(bindir) + 1 + strlen(name) );
    strcpy( argv[0], bindir );
    strcat( argv[0], "/" );
    strcat( argv[0], name );
    preloader_exec( argv, use_preloader );
}
