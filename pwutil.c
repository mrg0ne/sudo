/*
 * Copyright (c) 1996, 1998-2005 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Sponsored in part by the Defense Advanced Research Projects
 * Agency (DARPA) and Air Force Research Laboratory, Air Force
 * Materiel Command, USAF, under agreement number F39502-99-1-0512.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <stdio.h>
#ifdef STDC_HEADERS
# include <stdlib.h>
# include <stddef.h>
#else
# ifdef HAVE_STDLIB_H
#  include <stdlib.h>
# endif
#endif /* STDC_HEADERS */
#ifdef HAVE_STRING_H
# if defined(HAVE_MEMORY_H) && !defined(STDC_HEADERS)
#  include <memory.h>
# endif
# include <string.h>
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif /* HAVE_STRING_H */
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <pwd.h>
#include <grp.h>

#include "sudo.h"
#include "redblack.h"

#ifndef lint
static const char rcsid[] = "$Sudo$";
#endif /* lint */

#ifdef MYPW
extern void (*my_setgrent) __P((void));
extern void (*my_endgrent) __P((void));
extern struct group *(*my_getgrnam) __P((const char *));
extern struct group *(*my_getgrgid) __P((gid_t));
#define setgrent()	my_setgrent()
#define endgrent()	my_endgrent()
#define getgrnam(n)	my_getgrnam(n)
#define getgrgid(g)	my_getgrgid(g)

extern void (*my_setpwent) __P((void));
extern void (*my_endpwent) __P((void));
extern struct passwd *(*my_getpwnam) __P((const char *));
extern struct passwd *(*my_getpwuid) __P((uid_t));
#define setpwent()	my_setpwent()
#define endpwent()	my_endpwent()
#define getpwnam(n)	my_getpwnam(n)
#define getpwuid(u)	my_getpwuid(u)
#endif

/*
 * The passwd and group caches.
 */
static struct rbtree *pwcache_byuid, *pwcache_byname;
static struct rbtree *grcache_bygid, *grcache_byname;

static int  cmp_pwuid	__P((const VOID *, const VOID *));
static int  cmp_pwnam	__P((const VOID *, const VOID *));
static int  cmp_grgid	__P((const VOID *, const VOID *));
static int  cmp_grnam	__P((const VOID *, const VOID *));
static void pw_free	__P((VOID *));

/*
 * Compare by uid.
 */
static int
cmp_pwuid(v1, v2)
    const VOID *v1;
    const VOID *v2;
{
    const struct passwd *pw1 = (const struct passwd *) v1;
    const struct passwd *pw2 = (const struct passwd *) v2;
    return(pw1->pw_uid - pw2->pw_uid);
}

/*
 * Compare by user name.
 */
static int
cmp_pwnam(v1, v2)
    const VOID *v1;
    const VOID *v2;
{
    const struct passwd *pw1 = (const struct passwd *) v1;
    const struct passwd *pw2 = (const struct passwd *) v2;
    return(strcmp(pw1->pw_name, pw2->pw_name));
}

/*
 * Dynamically allocate space for a struct password and the constituent parts
 * that we care about.  Fills in pw_passwd from shadow file.
 */
static struct passwd *
sudo_pwdup(pw)
    const struct passwd *pw;
{
    char *cp;
    const char *pw_shell;
    size_t nsize, psize, csize, gsize, dsize, ssize, total;
    struct passwd *newpw;

    /* If shell field is empty, expand to _PATH_BSHELL. */
    pw_shell = (pw->pw_shell == NULL || pw->pw_shell[0] == '\0')
	? _PATH_BSHELL : pw->pw_shell;

    /* Allocate in one big chunk for easy freeing. */
    nsize = psize = csize = gsize = dsize = ssize = 0;
    total = sizeof(struct passwd);
    if (pw->pw_name) {
	    nsize = strlen(pw->pw_name) + 1;
	    total += nsize;
    }
    if (pw->pw_passwd) {
	    psize = strlen(pw->pw_passwd) + 1;
	    total += psize;
    }
#ifdef HAVE_LOGIN_CAP_H
    if (pw->pw_class) {
	    csize = strlen(pw->pw_class) + 1;
	    total += csize;
    }
#endif
    if (pw->pw_gecos) {
	    gsize = strlen(pw->pw_gecos) + 1;
	    total += gsize;
    }
    if (pw->pw_dir) {
	    dsize = strlen(pw->pw_dir) + 1;
	    total += dsize;
    }
    if (pw_shell) {
	    ssize = strlen(pw_shell) + 1;
	    total += ssize;
    }
    if ((cp = malloc(total)) == NULL)
	    return(NULL);
    newpw = (struct passwd *)cp;

    /*
     * Copy in passwd contents and make strings relative to space
     * at the end of the buffer.
     */
    (void)memcpy(newpw, pw, sizeof(struct passwd));
    cp += sizeof(struct passwd);
    if (nsize) {
	    (void)memcpy(cp, pw->pw_name, nsize);
	    newpw->pw_name = cp;
	    cp += nsize;
    }
    if (psize) {
	    (void)memcpy(cp, pw->pw_passwd, psize);
	    newpw->pw_passwd = cp;
	    cp += psize;
    }
#ifdef HAVE_LOGIN_CAP_H
    if (csize) {
	    (void)memcpy(cp, pw->pw_class, csize);
	    newpw->pw_class = cp;
	    cp += csize;
    }
#endif
    if (gsize) {
	    (void)memcpy(cp, pw->pw_gecos, gsize);
	    newpw->pw_gecos = cp;
	    cp += gsize;
    }
    if (dsize) {
	    (void)memcpy(cp, pw->pw_dir, dsize);
	    newpw->pw_dir = cp;
	    cp += dsize;
    }
    if (ssize) {
	    (void)memcpy(cp, pw_shell, ssize);
	    newpw->pw_shell = cp;
	    cp += ssize;
    }

    return(newpw);
}

/*
 * Get a password entry by uid and allocate space for it.
 * Fills in pw_passwd from shadow file if necessary.
 */
struct passwd *
sudo_getpwuid(uid)
    uid_t uid;
{
    struct passwd key, *pw;
    struct rbnode *node;

    key.pw_uid = uid;
    if ((node = rbfind(pwcache_byuid, &key)) != NULL) {
	pw = (struct passwd *) node->data;
	return(pw->pw_name != NULL ? pw : NULL);
    }
    /*
     * Cache passwd db entry if it exists or a negative response if not.
     */
    if ((pw = getpwuid(uid)) != NULL) {
	pw->pw_passwd = sudo_getepw(pw);	/* get shadow password */
	pw = sudo_pwdup(pw);
	if (rbinsert(pwcache_byname, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache user name, already exists");
	if (rbinsert(pwcache_byuid, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache uid, already exists");
	return(pw);
    } else {
	pw = emalloc(sizeof(*pw));
	memset(pw, 0, sizeof(*pw));
	pw->pw_uid = uid;
	if (rbinsert(pwcache_byuid, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache uid, already exists");
	return(NULL);
    }
}

/*
 * Get a password entry by name and allocate space for it.
 * Fills in pw_passwd from shadow file if necessary.
 */
struct passwd *
sudo_getpwnam(name)
    const char *name;
{
    struct passwd key, *pw;
    struct rbnode *node;
    size_t len;
    char *cp;

    key.pw_name = (char *) name;
    if ((node = rbfind(pwcache_byname, &key)) != NULL) {
	pw = (struct passwd *) node->data;
	return(pw->pw_uid != (uid_t) -1 ? pw : NULL);
    }
    /*
     * Cache passwd db entry if it exists or a negative response if not.
     */
    if ((pw = getpwnam(name)) != NULL) {
	pw->pw_passwd = sudo_getepw(pw);	/* get shadow password */
	pw = sudo_pwdup(pw);
	if (rbinsert(pwcache_byname, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache user name, already exists");
	if (rbinsert(pwcache_byuid, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache uid, already exists");
	return(pw);
    } else {
	len = strlen(name) + 1;
	cp = emalloc(sizeof(*pw) + len);
	memset(cp, 0, sizeof(*pw));
	pw = (struct passwd *) cp;
	cp += sizeof(*pw);
	memcpy(cp, name, len);
	pw->pw_name = cp;
	pw->pw_uid = (uid_t) -1;
	if (rbinsert(pwcache_byname, (VOID *) pw) != NULL)
	    errorx(1, "unable to cache user name, already exists");
	return(NULL);
    }
}

/*
 * Take a uid and return a faked up passwd struct.
 */
struct passwd *
sudo_fakepwuid(uid)
    uid_t uid;
{
    struct passwd *pw;
    struct rbnode *node;

    pw = emalloc(sizeof(struct passwd) + MAX_UID_T_LEN + 1);
    memset(pw, 0, sizeof(struct passwd));
    pw->pw_uid = uid;
    pw->pw_name = (char *)pw + sizeof(struct passwd);
    (void) snprintf(pw->pw_name, MAX_UID_T_LEN + 1, "#%lu",
	(unsigned long) uid);

    /* Store by uid and by name, overwriting cached version. */
    if ((node = rbinsert(pwcache_byuid, pw)) != NULL) {
	free(node->data);
	node->data = (VOID *) pw;
    }
    if ((node = rbinsert(pwcache_byname, pw)) != NULL) {
	free(node->data);
	node->data = (VOID *) pw;
    }
    return(pw);
}

/*
 * Take a uid in string form "#123" and return a faked up passwd struct.
 */
struct passwd *
sudo_fakepwnam(user)
    const char *user;
{
    struct passwd *pw;
    struct rbnode *node;
    size_t len;

    len = strlen(user);
    pw = emalloc(sizeof(struct passwd) + len + 1);
    memset(pw, 0, sizeof(struct passwd));
    pw->pw_uid = (uid_t) atoi(user + 1);
    pw->pw_name = (char *)pw + sizeof(struct passwd);
    strlcpy(pw->pw_name, user, len + 1);

    /* Store by uid and by name, overwriting cached version. */
    if ((node = rbinsert(pwcache_byuid, pw)) != NULL) {
	free(node->data);
	node->data = (VOID *) pw;
    }
    if ((node = rbinsert(pwcache_byname, pw)) != NULL) {
	free(node->data);
	node->data = (VOID *) pw;
    }
    return(pw);
}

void
sudo_setpwent()
{
    setpwent();
    sudo_setspent();
    pwcache_byuid = rbcreate(cmp_pwuid);
    pwcache_byname = rbcreate(cmp_pwnam);
}

void
sudo_endpwent()
{
    endpwent();
    sudo_endspent();
    rbdestroy(pwcache_byuid, pw_free);
    pwcache_byuid = NULL;
    rbdestroy(pwcache_byname, NULL);
    pwcache_byname = NULL;
}

static void
pw_free(v)
    VOID *v;
{
    struct passwd *pw = (struct passwd *) v;

    if (pw->pw_passwd != NULL)
	zero_bytes(pw->pw_passwd, strlen(pw->pw_passwd));
    free(pw);
}

/*
 * Compare by gid.
 */
static int
cmp_grgid(v1, v2)
    const VOID *v1;
    const VOID *v2;
{
    const struct group *grp1 = (const struct group *) v1;
    const struct group *grp2 = (const struct group *) v2;
    return(grp1->gr_gid - grp2->gr_gid);
}

/*
 * Compare by group name.
 */
static int
cmp_grnam(v1, v2)
    const VOID *v1;
    const VOID *v2;
{
    const struct group *grp1 = (const struct group *) v1;
    const struct group *grp2 = (const struct group *) v2;
    return(strcmp(grp1->gr_name, grp2->gr_name));
}

struct group *
sudo_grdup(gr)
    const struct group *gr;
{
    char *cp;
    size_t nsize, psize, csize, num, total, len;
    struct group *newgr;

    /* Allocate in one big chunk for easy freeing. */
    nsize = psize = csize = num = 0;
    total = sizeof(struct group);
    if (gr->gr_name) {
	    nsize = strlen(gr->gr_name) + 1;
	    total += nsize;
    }
    if (gr->gr_passwd) {
	    psize = strlen(gr->gr_passwd) + 1;
	    total += psize;
    }
    if (gr->gr_mem) {
	for (num = 0; gr->gr_mem[num] != NULL; num++)
	    total += strlen(gr->gr_mem[num]) + 1;
	num++;
	total += sizeof(char *) * num;
    }
    if ((cp = malloc(total)) == NULL)
	    return(NULL);
    newgr = (struct group *)cp;

    /*
     * Copy in group contents and make strings relative to space
     * at the end of the buffer.
     */
    (void)memcpy(newgr, gr, sizeof(struct group));
    cp += sizeof(struct group);
    if (nsize) {
	(void)memcpy(cp, gr->gr_name, nsize);
	newgr->gr_name = cp;
	cp += nsize;
    }
    if (psize) {
	(void)memcpy(cp, gr->gr_passwd, psize);
	newgr->gr_passwd = cp;
	cp += psize;
    }
    if (gr->gr_mem) {
	newgr->gr_mem = (char **)cp;
	cp += sizeof(char *) * num;
	for (num = 0; gr->gr_mem[num] != NULL; num++) {
	    len = strlen(gr->gr_mem[num]) + 1;
	    memcpy(cp, gr->gr_mem[num], len);
	    newgr->gr_mem[num] = cp;
	    cp += len;
	}
	newgr->gr_mem[num] = NULL;
    }

    return(newgr);
}

/*
 * Get a group entry by gid and allocate space for it.
 */
struct group *
sudo_getgrgid(gid)
    gid_t gid;
{
    struct group key, *gr;
    struct rbnode *node;

    key.gr_gid = gid;
    if ((node = rbfind(grcache_bygid, &key)) != NULL) {
	gr = (struct group *) node->data;
	return(gr->gr_name != NULL ? gr : NULL);
    }
    /*
     * Cache group db entry if it exists or a negative response if not.
     */
    if ((gr = getgrgid(gid)) != NULL) {
	gr = sudo_grdup(gr);
	if (rbinsert(grcache_byname, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache group name, already exists");
	if (rbinsert(grcache_bygid, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache gid, already exists");
	return(gr);
    } else {
	gr = emalloc(sizeof(*gr));
	memset(gr, 0, sizeof(*gr));
	gr->gr_gid = gid;
	if (rbinsert(grcache_bygid, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache gid, already exists");
	return(NULL);
    }
}

/*
 * Get a group entry by name and allocate space for it.
 */
struct group *
sudo_getgrnam(name)
    const char *name;
{
    struct group key, *gr;
    struct rbnode *node;
    size_t len;
    char *cp;

    key.gr_name = (char *) name;
    if ((node = rbfind(grcache_byname, &key)) != NULL) {
	gr = (struct group *) node->data;
	return(gr->gr_gid != (gid_t) -1 ? gr : NULL);
    }
    /*
     * Cache group db entry if it exists or a negative response if not.
     */
    if ((gr = getgrnam(name)) != NULL) {
	gr = sudo_grdup(gr);
	if (rbinsert(grcache_byname, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache group name, already exists");
	if (rbinsert(grcache_bygid, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache gid, already exists");
	return(gr);
    } else {
	len = strlen(name) + 1;
	cp = emalloc(sizeof(*gr) + len);
	memset(cp, 0, sizeof(*gr));
	gr = (struct group *) cp;
	cp += sizeof(*gr);
	memcpy(cp, name, len);
	gr->gr_name = cp;
	gr->gr_gid = (gid_t) -1;
	if (rbinsert(grcache_byname, (VOID *) gr) != NULL)
	    errorx(1, "unable to cache group name, already exists");
	return(NULL);
    }
}

void
sudo_setgrent()
{
    setgrent();
    grcache_bygid = rbcreate(cmp_grgid);
    grcache_byname = rbcreate(cmp_grnam);
}

void
sudo_endgrent()
{
    endgrent();
    rbdestroy(grcache_bygid, free);
    grcache_bygid = NULL;
    rbdestroy(grcache_byname, NULL);
    grcache_byname = NULL;
}
