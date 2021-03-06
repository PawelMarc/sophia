
/*
 * sophia database
 * sphia.org
 *
 * Copyright (c) Dmitry Simonenko
 * BSD License
*/

#include <sp.h>

static inline int sphot
cmpstd(char *a, size_t asz, char *b, size_t bsz, void *arg spunused) {
	register size_t sz = (asz < bsz ? asz : bsz);
	register int rc = memcmp(a, b, sz);
	return (rc == 0 ? rc : (rc > 0 ? 1 : -1));
}

static inline void sp_envinit(spenv *e) {
	e->m = SPMENV;
	e->inuse = 0;
	sp_einit(&e->e);
	e->alloc = sp_allocstd;
	e->allocarg = NULL;
	e->cmp = cmpstd;
	e->cmparg = NULL;
	e->page = 2048;
	e->dir = NULL;
	e->flags = 0;
	e->mergewm = 100000;
	e->merge = 1;
	e->dbnewsize = 2 * 1024 * 1024;
	e->dbgrow = 1.4;
	e->gc = 1;
	e->gcfactor = 0.5;
}

static inline void sp_envfree(spenv *e) {
	if (e->dir) {
		free(e->dir);
		e->dir = NULL;
	}
	sp_efree(&e->e);
}

static inline int sp_envvalidate(spenv *e)
{
	/* check if environment is not already
	 * in use.
	 * do not set other environment error status 
	 * in that case.
	 */
	if (e->inuse)
		return -1;
	if (e->dir == NULL)
		return sp_ee(e, SPE, "directory is not specified");
	if (e->mergewm < 2)
		return sp_ee(e, SPE, "bad merge watermark count");
	if (e->page < 2)
		return sp_ee(e, SPE, "bad page size");
	if ((e->page % 2) > 0)
		return sp_ee(e, SPE, "bad page size must be even");
	return 0;
}

void *sp_env(void) {
	spenv *e = malloc(sizeof(spenv));
	if (spunlikely(e == NULL))
		return NULL;
	sp_envinit(e);
	return e;
}

static int sp_ctlenv(spenv *e, spopt opt, va_list args)
{
	if (e->inuse)
		return sp_ee(e, SPEOOM, "can't change env opts while in-use");
	switch (opt) {
	case SPDIR: {
		uint32_t flags = va_arg(args, uint32_t);
		char *path = va_arg(args, char*);
		char *p = strdup(path);
		if (spunlikely(p == NULL))
			return sp_ee(e, SPEOOM, "failed to allocate memory");
		if (spunlikely(e->dir)) {
			free(e->dir);
			e->dir = NULL;
		}
		e->dir = p;
		e->flags = flags;
		break;
	}
	case SPALLOC:
		e->alloc = va_arg(args, spallocf);
		e->allocarg = va_arg(args, void*);
		break;
	case SPCMP:
		e->cmp = va_arg(args, spcmpf);
		e->cmparg = va_arg(args, void*);
		break;
	case SPPAGE:
		e->page = va_arg(args, uint32_t);
		break;
	case SPGC:
		e->gc = va_arg(args, int);
		break;
	case SPGCF:
		e->gcfactor = va_arg(args, double);
		break;
	case SPGROW:
		e->dbnewsize = va_arg(args, uint32_t);
		e->dbgrow = va_arg(args, double);
		break;
	case SPMERGE:
		e->merge = va_arg(args, int);
		break;
	case SPMERGEWM:
		e->mergewm = va_arg(args, uint32_t);
		break;
	default:
		return sp_ee(e, SPE, "bad arguments");
	}
	return 0;
}

static int sp_ctldb(sp *s, spopt opt, va_list args spunused)
{
	switch (opt) {
	case SPMERGEFORCE:
		if (s->env->merge)
			return sp_e(s, SPE, "force merge doesn't work with merger thread active");
		return sp_merge(s);
	default:
		return sp_e(s, SPE, "bad arguments");
	}
	return 0;
}

int sp_ctl(void *o, spopt opt, ...)
{
	va_list args;
	va_start(args, opt);
	spmagic *magic = (spmagic*)o;
	int rc;
	if (opt == SPVERSION) {
		uint32_t *major = va_arg(args, uint32_t*);
		uint32_t *minor = va_arg(args, uint32_t*);
		*major = SP_VERSION_MAJOR;
		*minor = SP_VERSION_MINOR;
		return 0;
	}
	switch (*magic) {
	case SPMENV: rc = sp_ctlenv(o, opt, args);
		break;
	case SPMDB: rc = sp_ctldb(o, opt, args);
		break;
	default: rc = -1;
		break;
	}
	va_end(args);
	return rc;
}

int sp_rotate(sp *s, spe *err)
{
	int rc;
	sp_repepochincrement(&s->rep);
	/* allocate new epoch */
	spepoch *e = sp_repalloc(&s->rep, sp_repepoch(&s->rep));
	if (spunlikely(s == NULL))
		return sp_ef(err, SPEOOM, "failed to allocate repository");
	/* create log file */
	rc = sp_lognew(&e->log, s->env->dir, sp_repepoch(&s->rep));
	if (spunlikely(rc == -1)) {
		sp_free(&s->a, e);
		return sp_ef(err, SPEIO, e->epoch, "failed to create log file");
	}
	splogh h;
	h.magic = SPMAGIC;
	h.version[0] = SP_VERSION_MAJOR;
	h.version[1] = SP_VERSION_MINOR;
	rc = sp_logwrite(&e->log, &h, sizeof(h));
	if (spunlikely(rc == -1)) {
		sp_logclose(&e->log);
		sp_free(&s->a, e);
		return sp_ef(err, SPEIO, e->epoch, "failed to write log file");
	}
	/* attach epoch and mark it is as live */
	sp_repattach(&s->rep, e);
	sp_repset(&s->rep, e, SPLIVE);
	return 0;
}

static inline int sp_closerep(sp *s)
{
	int rcret = 0;
	int rc = 0;
	splist *i, *n;
	sp_listforeach_safe(&s->rep.l, i, n) {
		spepoch *e = spcast(i, spepoch, link);
		switch (e->type) {
		case SPUNDEF:
			/* this type is true to a epoch that has beed
			 * scheduled for a recovery, but not happen to
			 * proceed yet. */
			break;
		case SPLIVE:
			if (e->nupdate == 0) {
				rc = sp_logunlink(&e->log);
				if (spunlikely(rc == -1))
					rcret = sp_e(s, SPEIO, e->epoch, "failed to unlink log file");
				rc = sp_logclose(&e->log);
				if (spunlikely(rc == -1))
					rcret = sp_e(s, SPEIO, e->epoch, "failed to close log file");
				break;
			} else {
				rc = sp_logeof(&e->log);
				if (spunlikely(rc == -1))
					rcret = sp_e(s, SPEIO, e->epoch, "failed to write eof marker");
			}
		case SPXFER:
			rc = sp_logcomplete(&e->log);
			if (spunlikely(rc == -1))
				rcret = sp_e(s, SPEIO, e->epoch, "failed to complete log file");
			rc = sp_logclose(&e->log);
			if (spunlikely(rc == -1))
				rcret = sp_e(s, SPEIO, e->epoch, "failed to close log file");
			break;
		case SPDB:
			rc = sp_mapclose(&e->db);
			if (spunlikely(rc == -1))
				rcret = sp_e(s, SPEIO, e->epoch, "failed to close db file");
			break;
		}
		sp_free(&s->a, e);
	}
	return rcret;
}

static inline int sp_close(sp *s)
{
	int rcret = 0;
	int rc = 0;
	s->stop = 1;
	if (s->env->merge) {
		rc = sp_taskstop(&s->merger);
		if (spunlikely(rc == -1))
			rcret = sp_e(s, SPESYS, "failed to stop merger thread");
	}
	sp_refsetfree(&s->refs, &s->a);
	rc = sp_closerep(s);
	if (spunlikely(rc == -1))
		rcret = -1;
	rc = sp_recoverunlock(s);
	if (spunlikely(rc == -1))
		rcret = -1;
	sp_ifree(&s->i0);
	sp_ifree(&s->i1);
	sp_ifree(&s->itxn); /* equal to rollback */
	sp_catfree(&s->s);
	s->env->inuse = 0;
	sp_lockfree(&s->lockr);
	sp_lockfree(&s->locks);
	sp_lockfree(&s->locki);
	sp_efree(&s->e);
	sp_efree(&s->em);
	return rcret;
}

static void *merger(void *arg)
{
	sptask *self = arg;
	sp *s = self->arg;
	do {
		sp_lock(&s->locki);
		int merge = s->i->count > s->env->mergewm;
		sp_unlock(&s->locki);
		if (! merge)
			continue;
		int rc = sp_merge(s);
		if (spunlikely(rc == -1)) {
			sp_taskdone(self);
			return NULL;
		}
	} while (sp_taskwait(self));

	return NULL;
}

void *sp_open(void *e)
{
	spenv *env = e;
	assert(env->m == SPMENV);
	int rc = sp_envvalidate(env);
	if (spunlikely(rc == -1))
		return NULL;
	spa a;
	sp_allocinit(&a, env->alloc, env->allocarg);
	sp *s = sp_malloc(&a, sizeof(sp));
	if (spunlikely(s == NULL)) {
		sp_ee(env, SPEOOM, "failed to allocate db handle");
		return NULL;
	}
	memset(s, 0, sizeof(sp));
	sp_einit(&s->e);
	sp_einit(&s->em);
	s->m = SPMDB;
	s->env = env;
	s->env->inuse = 1;
	memcpy(&s->a, &a, sizeof(s->a));
	/* init locks */
	sp_fileinit(&s->lockdb, &s->a);
	sp_lockinit(&s->lockr);
	sp_lockinit(&s->locks);
	sp_lockinit(&s->locki);
	s->lockc = 0;
	/* init key index */
	rc = sp_iinit(&s->i0, &s->a, 1024, s->env->cmp, s->env->cmparg);
	if (spunlikely(rc == -1)) {
		sp_e(s, SPEOOM, "failed to allocate key index");
		goto e0;
	}
	rc = sp_iinit(&s->i1, &s->a, 1024, s->env->cmp, s->env->cmparg);
	if (spunlikely(rc == -1)) {
		sp_e(s, SPEOOM, "failed to allocate key index");
		goto e1;
	}
	s->i = &s->i0;
	/* init transaction index */
	rc = sp_iinit(&s->itxn, &s->a, 1024, s->env->cmp, s->env->cmparg);
	if (spunlikely(rc == -1)) {
		sp_e(s, SPEOOM, "failed to allocate transaction index");
		goto e2;
	}
	/* set current transaction state as single-stmt */
	s->txn = SPTSS;
	/* init page index */
	s->psn = 0;
	rc = sp_catinit(&s->s, &s->a, 512, s->env->cmp, s->env->cmparg);
	if (spunlikely(rc == -1)) {
		sp_e(s, SPEOOM, "failed to allocate page index");
		goto e2;
	}
	sp_repinit(&s->rep, &s->a);
	rc = sp_recover(s);
	if (spunlikely(rc == -1))
		goto e3;
	/* do not create new live epoch in read-only mode */
	if (! (s->env->flags & SPO_RDONLY)) {
		rc = sp_rotate(s, &s->e);
		if (spunlikely(rc == -1))
			goto e3;
	}
	s->stop = 0;
	rc = sp_refsetinit(&s->refs, &s->a, s->env->page);
	if (spunlikely(rc == -1)) {
		sp_e(s, SPEOOM, "failed to allocate key buffer");
		goto e3;
	}
	if (s->env->merge) {
		rc = sp_taskstart(&s->merger, merger, s);
		if (spunlikely(rc == -1)) {
			sp_e(s, SPESYS, "failed to start merger thread");
			goto e4;
		}
		sp_taskwakeup(&s->merger);
	}
	return s;
e4:
	sp_refsetfree(&s->refs, &s->a);
e3:
	sp_closerep(s);
	sp_recoverunlock(s);
	sp_catfree(&s->s);
e2:
	sp_ifree(&s->itxn);
	sp_ifree(&s->i1);
e1:
	sp_ifree(&s->i0);
e0:
	s->env->inuse = 0;
	sp_lockfree(&s->lockr);
	sp_lockfree(&s->locks);
	sp_lockfree(&s->locki);
	sp_edup(&env->e, &s->e);
	sp_efree(&s->e);
	sp_efree(&s->em);
	sp_free(&a, s);
	return NULL;
}

int sp_destroy(void *o)
{
	spmagic *magic = (spmagic*)o;
	spa *a = NULL;
	int rc = 0;
	switch (*magic) {
	case SPMNONE:
		assert(0);
		return -1;
	case SPMENV: {
		spenv *env = (spenv*)o;
		if (env->inuse)
			return -1;
		sp_envfree(env);
		*magic = SPMNONE;
		free(o);
		return 0;
	}
	case SPMCUR: {
		spc *c = (spc*)o;
		a = &c->s->a;
		sp_cursorclose(c);
		break;
	}
	case SPMDB: {
		sp *s = (sp*)o;
		a = &s->a;
		rc = sp_close(s);
		break;
	}
	default:
		return -1;
	}
	*magic = SPMNONE;
	sp_free(a, o);
	return rc;
}

char *sp_error(void *o)
{
	spmagic *magic = (spmagic*)o;
	spenv *env;
	switch (*magic) {
	case SPMENV:
		env = o;
		if (! sp_eis(&env->e))
			return NULL;
		return env->e.e;
	case SPMDB: break;
	default:
		assert(0);
		return NULL;
	}
	sp *s = o;
	if (sp_eis(&s->em))
		return s->em.e;
	if (sp_eis(&s->e))
		return s->e.e;
	return NULL;
}

int sp_begin(void *o)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(s->txn == SPTMS))
		return -1;
	if (spunlikely(s->lockc))
		return sp_e(s, SPE, "begin with open cursor");
	s->txn = SPTMS;
	return 0;
}

int sp_commit(void *o)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(s->txn == SPTSS))
		return sp_e(s, SPE, "no active transaction to commit");
	if (spunlikely(s->lockc))
		return sp_e(s, SPE, "commit with open cursor");
	if (spunlikely(s->itxn.count == 0)) {
		s->txn = SPTSS;
		return 0;
	}

	/* prepare to write the transaction
	 * to the log */
	int n = s->itxn.count;

	sp_lock(&s->lockr);
	sp_lock(&s->locki);

	spepoch *live = sp_replive(&s->rep);
	sp_filesvp(&live->log);

	char hbuf[sizeof(spvh) * 512];
	unsigned int hpos = 0;

	spii it;
	sp_iopen(&it, &s->itxn);
	int rc;
	do {
		spv *v = sp_ival(&it);

		if (spunlikely(! sp_batchensure(&s->lb, 3))) {
			rc = sp_logput(&live->log, &s->lb);
			if (spunlikely(rc == -1)) {
				sp_e(s, SPEIO|SPEF, live->epoch, "failed to write log file");
				goto abort;
			}
			hpos = 0;
		}

		v->epoch = live->epoch;
		assert(hpos < sizeof(hbuf));
		spvh *hp = (spvh*)(hbuf + hpos);
		hp->crc     = 0;
		hp->size    = v->size;
		hp->voffset = 0;
		hp->vsize   = sp_vvsize(v);
		hp->flags   = v->flags;
		hp->crc     = sp_crc32c(v->crc, &hp->size, sizeof(spvh) - sizeof(uint32_t));
		sp_batchadd(&s->lb, hp, sizeof(spvh));
		sp_batchadd(&s->lb, v->key, v->size);
		sp_batchadd(&s->lb, sp_vv(v), hp->vsize);
		hpos += sizeof(spvh);

		spv *old = NULL;
		rc = sp_iset(s->i, v, &old);
		if (spunlikely(rc == -1)) {
			sp_e(s, SPEOOM|SPEF, "failed to allocate key index page");
			goto abort;
		}
		if (old)
			sp_free(&s->a, old);

	} while (sp_inext(&it));

	if (sp_batchhas(&s->lb)) {
		rc = sp_logput(&live->log, &s->lb);
		if (spunlikely(rc == -1)) {
			sp_e(s, SPEIO|SPEF, live->epoch, "failed to write log file");
			goto abort;
		}
	}

	/* clean up transaction index (pages only) */
	sp_ireset(&s->itxn);

	sp_unlock(&s->locki);
	sp_unlock(&s->lockr);

	/* set transaction as single-stmt */
	s->txn = SPTSS;

	/* wake up merger if necessary */
	live->nupdate += n;
	if (live->nupdate >= s->env->mergewm) {
		if (splikely(s->env->merge))
			sp_taskwakeup(&s->merger);
	}
	return 0;

abort:
	sp_rollback(o);
	sp_logrlb(&live->log);
	sp_unlock(&s->locki);
	sp_unlock(&s->lockr);
	return -1;
}

int sp_rollback(void *o)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(s->txn == SPTSS))
		return sp_e(s, SPE, "no active transaction to rollback");
	if (spunlikely(s->lockc))
		return sp_e(s, SPE, "rollback with open cursor");
	int rc = sp_itruncate(&s->itxn);
	if (spunlikely(rc == -1))
		return sp_e(s, SPEOOM, "failed to allocate key index page");
	s->txn = SPTSS;
	return 0;
}

static inline int
sp_do(sp *s, int op, void *k, size_t ksize, void *v, size_t vsize)
{
	/* allocate new version.
	 *
	 * try to reduce lock contention by making the alloc and
	 * the crc calculation before log write. 
	*/
	spv *n = sp_vnewv(s, k, ksize, v, vsize);
	if (spunlikely(n == NULL))
		return sp_e(s, SPEOOM, "failed to allocate version");
	/* prepare log record */
	spvh h = {
		.crc     = 0,
		.size    = ksize,
		.voffset = 0,
		.vsize   = vsize,
		.flags   = op 
	};
	/* calculate crc */
	uint32_t crc;
 	crc   = sp_crc32c(0, k, ksize);
	crc   = sp_crc32c(crc, v, vsize);
	h.crc = sp_crc32c(crc, &h.size, sizeof(spvh) - sizeof(uint32_t));

	n->flags = op;
	n->crc = crc;

	/* in case of multi-stmt transaction, simply add version to the
	 * transaction index only. */
	int rc;
	if (s->txn == SPTMS) {
		spv *old = NULL;
		rc = sp_iset(&s->itxn, n, &old);
		if (spunlikely(rc == -1)) {
			sp_free(&s->a, n);
			return sp_e(s, SPEOOM, "failed to allocate transacton key index page");
		}
		if (old)
			sp_free(&s->a, old);
		return 0;
	}

	sp_lock(&s->lockr);
	sp_lock(&s->locki);

	/* write to current live epoch log */
	spepoch *live = sp_replive(&s->rep);
	sp_filesvp(&live->log);
	sp_batchadd(&s->lb, &h, sizeof(spvh));
	sp_batchadd(&s->lb, k, ksize);
	sp_batchadd(&s->lb, v, vsize);
	rc = sp_logput(&live->log, &s->lb);
	if (spunlikely(rc == -1)) {
		sp_free(&s->a, n);
		rc = sp_logrlb(&live->log);
		if (spunlikely(rc == -1))
			sp_esetfatal(&s->e);
		sp_unlock(&s->locki);
		sp_unlock(&s->lockr);
		return sp_e(s, SPEIO, live->epoch, "failed to write log file");
	}

	/* add new version to the index */
	n->epoch = live->epoch;
	spv *old = NULL;
	rc = sp_iset(s->i, n, &old);
	if (spunlikely(rc == -1)) {
		sp_free(&s->a, n);
		rc = sp_logrlb(&live->log);
		if (spunlikely(rc == -1))
			sp_esetfatal(&s->e);
		sp_unlock(&s->locki);
		sp_unlock(&s->lockr);
		return (spunlikely(rc == -1)) ? -1 :
		        sp_e(s, SPEOOM, "failed to allocate key index page");
	}

	sp_unlock(&s->locki);
	sp_unlock(&s->lockr);

	if (old)
		sp_free(&s->a, old);

	/* wake up merger on merge watermark reached */
	live->nupdate++;
	if ((live->nupdate % s->env->mergewm) == 0) {
		if (splikely(s->env->merge))
			sp_taskwakeup(&s->merger);
	}
	return 0;
}

int sp_set(void *o, const void *k, size_t ksize, const void *v, size_t vsize)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(s->env->flags & SPO_RDONLY))
		return sp_e(s, SPE, "db handle is read-only");
	if (spunlikely(ksize > UINT16_MAX))
		return sp_e(s, SPE, "key size limit reached");
	if (spunlikely(vsize > UINT32_MAX))
		return sp_e(s, SPE, "value size limit reached");
	if (spunlikely(s->lockc))
		return sp_e(s, SPE, "modify with open cursor");
	return sp_do(s, SPSET, (char*)k, ksize, (char*)v, vsize);
}

int sp_delete(void *o, const void *k, size_t ksize)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(s->env->flags & SPO_RDONLY))
		return sp_e(s, SPE, "db handle is read-only");
	if (spunlikely(ksize > UINT16_MAX))
		return sp_e(s, SPE, "key size limit reached");
	if (spunlikely(s->lockc))
		return sp_e(s, SPE, "modify with open cursor");
	return sp_do(s, SPDEL, (char*)k, ksize, NULL, 0);
}

int sp_get(void *o, const void *k, size_t ksize, void **v, size_t *vsize)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return -1;
	if (spunlikely(ksize > UINT16_MAX))
		return sp_e(s, SPE, "key size limit reached");
	return sp_match(s, (char*)k, ksize, v, vsize);
}

void *sp_cursor(void *o, sporder order, const void *k, size_t ksize)
{
	sp *s = o;
	assert(s->m == SPMDB);
	if (spunlikely(sp_evalidate(s)))
		return NULL;
	if (spunlikely(ksize > UINT16_MAX)) {
		sp_e(s, SPE, "key size limit reached");
		return NULL;
	}
	spc *c = sp_malloc(&s->a, sizeof(spc));
	if (spunlikely(c == NULL)) {
		sp_e(s, SPEOOM, "failed to allocate cursor handle");
		return NULL;
	}
	memset(c, 0, sizeof(spc));
	sp_cursoropen(c, s, order, (char*)k, ksize);
	return c;
}

int sp_fetch(void *o) {
	spc *c = o;
	assert(c->m == SPMCUR);
	if (spunlikely(sp_evalidate(c->s)))
		return -1;
	return sp_iterate(c);
}

const char *sp_key(void *o)
{
	spc *c = o;
	assert(c->m == SPMCUR);
	return sp_refk(&c->r);
}

size_t sp_keysize(void *o)
{
	spc *c = o;
	assert(c->m == SPMCUR);
	return sp_refksize(&c->r);
}

const char *sp_value(void *o)
{
	spc *c = o;
	assert(c->m == SPMCUR);
	return sp_refv(&c->r, (char*)c->ph);
}

size_t sp_valuesize(void *o)
{
	spc *c = o;
	assert(c->m == SPMCUR);
	return sp_refvsize(&c->r);
}

void sp_stat(void *o, spstat *stat)
{
	spmagic *magic = (spmagic*)o;
	if (*magic != SPMDB) {
		memset(stat, 0, sizeof(*stat));
		return;
	}
	sp *s = o;
	sp_lock(&s->lockr);
	sp_lock(&s->locki);
	sp_lock(&s->locks);
	stat->epoch = s->rep.epoch;
	stat->psn = s->psn;
	stat->repn = s->rep.n;
	stat->repndb = s->rep.ndb;
	stat->repnxfer = s->rep.nxfer;
	stat->catn = s->s.count;
	stat->indexn = s->i->count;
	stat->indexpages = s->i->icount;
	sp_unlock(&s->locks);
	sp_unlock(&s->locki);
	sp_unlock(&s->lockr);
}
