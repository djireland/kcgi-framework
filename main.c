/*	$Id$ */
/*
 * Copyright (c) 2016 Kristaps Dzonsons <kristaps@bsd.lv>
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
 */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <kcgi.h>
#include <kcgijson.h>
#include <ksql.h>

/*
 * A user.
 * See the user table in the database schema.
 */
struct	user {
	char		*email;
	int64_t		 id;
};

/*
 * Start with five pages.
 * As you add more pages, you'll start by giving them an identifier key
 * in this enum.
 */
enum	page {
	PAGE_INDEX,
	PAGE_LOGIN,
	PAGE_LOGOUT,
	PAGE_USER_MOD_EMAIL,
	PAGE_USER_MOD_PASS,
	PAGE__MAX
};

enum	key {
	KEY_EMAIL,
	KEY_PASS,
	KEY_SESSTOK,
	KEY_SESSID,
	KEY__MAX
};

enum	stmt {
	STMT_SESS_DEL,
	STMT_SESS_GET,
	STMT_SESS_NEW,
	STMT_USER_GET,
	STMT_USER_LOOKUP,
	STMT_USER_MOD_EMAIL,
	STMT_USER_MOD_HASH,
	STMT__MAX
};

/*
 * The fields we'll extract (in general) from the user table.
 */
#define	USER	"user.id,user.email"

static	const char *const stmts[STMT__MAX] = {
	/* STMT_SESS_DEL */
	"DELETE FROM sess WHERE id=? AND token=? AND userid=?",
	/* STMT_SESS_GET */
	"SELECT " USER " FROM sess "
		"INNER JOIN user ON user.id=sess.userid "
		"WHERE sess.id=? AND sess.token=?",
	/* STMT_SESS_NEW */
	"INSERT INTO sess (token,userid) VALUES (?,?)",
	/* STMT_USER_GET */
	"SELECT " USER " FROM user WHERE id=?",
	/* STMT_USER_LOOKUP */
	"SELECT " USER ",hash FROM user WHERE email=?",
	/* STMT_USER_MOD_EMAIL */
	"UPDATE user SET email=? WHERE id=?",
	/* STMT_USER_MOD_HASH */
	"UPDATE user SET hash=? WHERE id=?",
};

static const struct kvalid keys[KEY__MAX] = {
	{ kvalid_email, "email" }, /* KEY_EMAIL */
	{ kvalid_stringne, "pass" }, /* KEY_PASS */
	{ kvalid_uint, "stok" }, /* KEY_SESSTOK */
	{ kvalid_int, "sid" }, /* KEY_SESSID */
};

static const char *const pages[PAGE__MAX] = {
	"index", /* PAGE_INDEX */
	"login", /* PAGE_LOGIN */
	"logout", /* PAGE_LOGOUT */
	"usermodemail", /* PAGE_USER_MOD_EMAIL */
	"usermodpass", /* PAGE_USER_MOD_PASS */
};

/*
 * Fill out all HTTP secure headers.
 * Use the existing document's MIME type.
 */
static void
http_alloc(struct kreq *r, enum khttp code)
{

	khttp_head(r, kresps[KRESP_STATUS], 
		"%s", khttps[code]);
	khttp_head(r, kresps[KRESP_CONTENT_TYPE], 
		"%s", kmimetypes[r->mime]);
	khttp_head(r, "X-Content-Type-Options", "nosniff");
	khttp_head(r, "X-Frame-Options", "DENY");
	khttp_head(r, "X-XSS-Protection", "1; mode=block");
}

/*
 * Fill out all headers with http_alloc() then start the HTTP document
 * body (no more headers after this point!)
 */
static void
http_open(struct kreq *r, enum khttp code)
{

	http_alloc(r, code);
	khttp_body(r);
}

/*
 * Free the contents of the "struct user".
 */
static void
db_user_unfill(struct user *p)
{

	if (NULL == p)
		return;
	free(p->email);
}

/*
 * Free the object of the "struct user".
 */
static void
db_user_free(struct user *p)
{

	db_user_unfill(p);
	free(p);
}

/*
 * Zero and fill the "struct user" from the database.
 * Sets "pos", if non-NULL, to be the current index in the database
 * columns using the USER column definition macro.
 */
static void
db_user_fill(struct user *p, struct ksqlstmt *stmt, size_t *pos)
{
	size_t	 i = 0;

	if (NULL == pos)
		pos = &i;

	memset(p, 0, sizeof(struct user));
	p->id = ksql_stmt_int(stmt, (*pos)++);
	p->email = kstrdup(ksql_stmt_str(stmt, (*pos)++));
}

/*
 * Create a new user session with a random token.
 * Returns the identifier of the new session.
 */
static int64_t
db_sess_new(struct kreq *r, int64_t token, const struct user *u)
{
	struct ksqlstmt	*stmt;
	int64_t		 id;

	ksql_stmt_alloc(r->arg, &stmt, 
		stmts[STMT_SESS_NEW], 
		STMT_SESS_NEW);
	ksql_bind_int(stmt, 0, token);
	ksql_bind_int(stmt, 1, u->id);
	ksql_stmt_step(stmt);
	ksql_stmt_free(stmt);
	ksql_lastid(r->arg, &id);
	kutil_info(r, u->email, "new session");
	return(id);
}

/*
 * Look up a given user with an e-mail and password.
 * The password is hashed in a system-dependent way.
 * Returns the user object or NULL if no user was found or the password
 * was incorrect.
 */
static struct user *
db_user_find(struct ksql *sql, const char *email, const char *pass)
{
	struct ksqlstmt	*stmt;
	int		 rc;
	size_t		 i;
	const char	*hash;
	struct user	*user;

	ksql_stmt_alloc(sql, &stmt, 
		stmts[STMT_USER_LOOKUP], 
		STMT_USER_LOOKUP);
	ksql_bind_str(stmt, 0, email);
	if (KSQL_ROW != ksql_stmt_step(stmt)) {
		ksql_stmt_free(stmt);
		return(NULL);
	}
	i = 0;
	user = kmalloc(sizeof(struct user));
	db_user_fill(user, stmt, &i);
	hash = ksql_stmt_str(stmt, i);
#ifdef __OpenBSD__
	rc = crypt_checkpass(pass, hash) < 0 ? 0 : 1;
#else
	rc = 0 == strcmp(hash, pass);
#endif
	ksql_stmt_free(stmt);
	if (0 == rc) {
		db_user_free(user);
		user = NULL;
	}
	return(user);
}

/*
 * Resolve a user from session information.
 * Returns the user object or NULL if no session was found.
 */
static struct user *
db_sess_resolve(struct kreq *r, int64_t id, int64_t token)
{
	struct ksqlstmt	*stmt;
	struct user	*u = NULL;

	if (-1 == id || -1 == token)
		return(NULL);

	ksql_stmt_alloc(r->arg, &stmt, 
		stmts[STMT_SESS_GET], 
		STMT_SESS_GET);
	ksql_bind_int(stmt, 0, id);
	ksql_bind_int(stmt, 1, token);
	if (KSQL_ROW == ksql_stmt_step(stmt)) {
		u = kmalloc(sizeof(struct user));
		db_user_fill(u, stmt, NULL);
	}
	ksql_stmt_free(stmt);
	return(u);
}

/*
 * Deletes the session (if any) associated with the given id and token,
 * and managed by the given user.
 */
static void
db_sess_del(struct kreq *r, 
	const struct user *u, int64_t id, int64_t token)
{
	struct ksqlstmt	*stmt;

	ksql_stmt_alloc(r->arg, &stmt, 
		stmts[STMT_SESS_DEL], 
		STMT_SESS_DEL);
	ksql_bind_int(stmt, 0, id);
	ksql_bind_int(stmt, 1, token);
	ksql_bind_int(stmt, 2, u->id);
	ksql_stmt_step(stmt);
	ksql_stmt_free(stmt);
	kutil_info(r, u->email, "session deleted");
}

/*
 * Modify a user's password.
 */
static void
db_user_mod_pass(struct kreq *r, 
	const struct user *u, const char *pass)
{
	struct ksqlstmt	*stmt;
	char		 hash[64];

#ifdef __OpenBSD__
	crypt_newhash(pass, "blowfish,a", hash, sizeof(hash));
#else
	strlcpy(hash, pass, sizeof(hash));
#endif
	ksql_stmt_alloc(r->arg, &stmt,
		stmts[STMT_USER_MOD_HASH],
		STMT_USER_MOD_HASH);
	ksql_bind_str(stmt, 0, hash);
	ksql_bind_int(stmt, 1, u->id);
	ksql_stmt_step(stmt);
	ksql_stmt_free(stmt);
	kutil_info(r, u->email, "changed password");
}

/*
 * Modify a user's e-mail.
 */
static int
db_user_mod_email(struct kreq *r, 
	const struct user *u, const char *email)
{
	struct ksqlstmt	*stmt;
	enum ksqlc	 c;

	ksql_stmt_alloc(r->arg, &stmt, 
		stmts[STMT_USER_MOD_EMAIL], 
		STMT_USER_MOD_EMAIL);
	ksql_bind_str(stmt, 0, email);
	ksql_bind_int(stmt, 1, u->id);
	c = ksql_stmt_cstep(stmt);
	ksql_stmt_free(stmt);
	if (KSQL_CONSTRAINT != c)
		kutil_info(r, u->email, "changed email: %s", email);
	return(KSQL_CONSTRAINT != c);
}

/*
 * Formats a user object's members as JSON members.
 */
static void
json_putuserdata(struct kjsonreq *req, const struct user *u)
{

	kjson_putstringp(req, "email", u->email);
	kjson_putintp(req, "id", u->id);
}

/*
 * Formats a user object as a JSON object.
 */
static void
json_putuser(struct kjsonreq *req, const struct user *u)
{

	kjson_objp_open(req, "user");
	json_putuserdata(req, u);
	kjson_obj_close(req);
}

/*
 * Process an e-mail address change.
 * Raises HTTP 400 if not all fields exist or if the e-mail address is
 * already taken by the system.
 * Raises HTTP 200 on success.
 */
static void
sendmodemail(struct kreq *r, const struct user *u)
{
	struct kpair	*kp;
	int		 rc;

	if (NULL != (kp = r->fieldmap[KEY_EMAIL])) {
		rc = db_user_mod_email(r, u, kp->parsed.s);
		http_open(r, rc ? KHTTP_200 : KHTTP_400);
	} else
		http_open(r, KHTTP_400);
}

/*
 * Process a password change.
 * Raises HTTP 400 if not all fields exist.
 * Raises HTTP 200 on success.
 */
static void
sendmodpass(struct kreq *r, const struct user *u)
{
	struct kpair	*kp;

	if (NULL != (kp = r->fieldmap[KEY_PASS])) {
		http_open(r, KHTTP_200);
		db_user_mod_pass(r, u, kp->parsed.s);
	} else
		http_open(r, KHTTP_400);
}

/*
 * Retrieve user information.
 * Raises HTTP 200 on success and the JSON of the user.
 */
static void
sendindex(struct kreq *r, const struct user *u)
{
	struct kjsonreq	 req;

	http_open(r, KHTTP_200);
	kjson_open(&req, r);
	kjson_obj_open(&req);
	json_putuser(&req, u);
	kjson_obj_close(&req);
	kjson_close(&req);
}

static void
sendlogin(struct kreq *r)
{
	int64_t		 sid, token;
	struct kpair	*kpi, *kpp;
	char		 buf[64];
	struct user	*u;
	const char	*secure;

	if (NULL == (kpi = r->fieldmap[KEY_EMAIL]) ||
	    NULL == (kpp = r->fieldmap[KEY_PASS])) {
		http_open(r, KHTTP_400);
		return;
	}

	u = db_user_find(r->arg, 
		kpi->parsed.s, kpp->parsed.s);

	if (NULL == u) {
		http_open(r, KHTTP_400);
		return;
	} 

	token = arc4random();
	sid = db_sess_new(r, token, u);
	kutil_epoch2str
		(time(NULL) + 60 * 60 * 24 * 365,
		 buf, sizeof(buf));
#ifdef SECURE
	secure = " secure;";
#else
	secure = "";
#endif
	khttp_head(r, kresps[KRESP_STATUS], 
		"%s", khttps[KHTTP_200]);
	khttp_head(r, kresps[KRESP_SET_COOKIE],
		"%s=%" PRId64 ";%s HttpOnly; path=/; expires=%s", 
		keys[KEY_SESSTOK].name, token, secure, buf);
	khttp_head(r, kresps[KRESP_SET_COOKIE],
		"%s=%" PRId64 ";%s HttpOnly; path=/; expires=%s", 
		keys[KEY_SESSID].name, sid, secure, buf);
	khttp_body(r);

	db_user_free(u);
}

static void
sendlogout(struct kreq *r, const struct user *u)
{
	const char	*secure;
	char		 buf[32];

	kutil_epoch2str(0, buf, sizeof(buf));
#ifdef SECURE
	secure = " secure;";
#else
	secure = "";
#endif

	http_alloc(r, KHTTP_200);
	khttp_head(r, kresps[KRESP_SET_COOKIE],
		"%s=; path=/;%s HttpOnly; expires=%s", 
		keys[KEY_SESSTOK].name, secure, buf);
	khttp_head(r, kresps[KRESP_SET_COOKIE],
		"%s=; path=/;%s HttpOnly; expires=%s", 
		keys[KEY_SESSID].name, secure, buf);
	khttp_body(r);
	db_sess_del(r, u,
		r->cookiemap[KEY_SESSID]->parsed.i, 
		r->cookiemap[KEY_SESSTOK]->parsed.i);
}

int
main(void)
{
	struct kreq	 r;
	enum kcgi_err	 er;
	struct ksql	*sql;
	struct ksqlcfg	 cfg;
	struct user	*u;

	/* Log into a separate logfile (not system log). */

	kutil_openlog(LOGFILE);

	/* Configure normal database except with foreign keys. */

	memset(&cfg, 0, sizeof(struct ksqlcfg));
	cfg.flags = KSQL_EXIT_ON_ERR |
		    KSQL_FOREIGN_KEYS |
		    KSQL_SAFE_EXIT;
	cfg.err = ksqlitemsg;
	cfg.dberr = ksqlitedbmsg;

	/* Actually parse HTTP document. */

	er = khttp_parse(&r, keys, KEY__MAX, 
		pages, PAGE__MAX, PAGE_INDEX);

	if (KCGI_OK != er) {
		fprintf(stderr, "HTTP parse error: %d\n", er);
		return(EXIT_FAILURE);
	}

#ifdef	__OpenBSD__
	if (-1 == pledge("stdio rpath cpath wpath flock fattr", NULL)) {
		kutil_warn(&r, NULL, "pledge");
		khttp_free(&r);
		return(EXIT_FAILURE);
	}
#endif

	/*
	 * Front line of defence: make sure we're a proper method, make
	 * sure we're a page, make sure we're a JSON file.
	 */

	if (KMETHOD_GET != r.method && 
	    KMETHOD_POST != r.method) {
		http_open(&r, KHTTP_405);
		khttp_free(&r);
		return(EXIT_SUCCESS);
	} else if (PAGE__MAX == r.page || 
	           KMIME_APP_JSON != r.mime) {
		http_open(&r, KHTTP_404);
		khttp_puts(&r, "Page not found.");
		khttp_free(&r);
		return(EXIT_SUCCESS);
	}

	/* Allocate database. */

	if (NULL == (sql = ksql_alloc(&cfg))) {
		http_open(&r, KHTTP_500);
		khttp_free(&r);
		return(EXIT_SUCCESS);
	} 

	ksql_open(sql, DATADIR "/" DATABASE);
	r.arg = sql;

	/* 
	 * Assume we're logging in with a session and grab the session
	 * from the database.
	 * This is our first database access.
	 */

	u = db_sess_resolve(&r,
		NULL != r.cookiemap[KEY_SESSID] ?
		r.cookiemap[KEY_SESSID]->parsed.i : -1,
		NULL != r.cookiemap[KEY_SESSTOK] ?
		r.cookiemap[KEY_SESSTOK]->parsed.i : -1);

	/* User authorisation. */

	if (PAGE_LOGIN != r.page && NULL == u) {
		http_open(&r, KHTTP_403);
		khttp_free(&r);
		ksql_free(sql);
		return(EXIT_SUCCESS);
	}

	switch (r.page) {
	case (PAGE_INDEX):
		sendindex(&r, u);
		break;
	case (PAGE_LOGIN):
		sendlogin(&r);
		break;
	case (PAGE_LOGOUT):
		sendlogout(&r, u);
		break;
	case (PAGE_USER_MOD_EMAIL):
		sendmodemail(&r, u);
		break;
	case (PAGE_USER_MOD_PASS):
		sendmodpass(&r, u);
		break;
	default:
		abort();
	}

	khttp_free(&r);
	ksql_free(sql);
	return(EXIT_SUCCESS);
}