/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * XXX:
 *	Better error messages, throughout.
 *	>It also occurred to me that we could link the errors to the error
 *	>documentation.
 *	>
 *	>Unreferenced  function 'request_policy', first mention is
 *	>         Line 8 Pos 4
 *	>         sub request_policy {
 *	>         ----##############--
 *	>Read more about this type of error:
 *	>http://varnish/doc/error.html#Unreferenced%20function
 *	>
 *	>
 *	>         Unknown variable 'obj.bandwidth'
 *	>         At: Line 88 Pos 12
 *	>                 if (obj.bandwidth < 1 kb/h) {
 *	>         ------------#############------------
 *	>Read more about this type of error:
 *	>http://varnish/doc/error.html#Unknown%20variable
 *
 */

#include "config.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vcc_compile.h"

#include "libvcc.h"
#include "vfil.h"

static const struct method method_tab[] = {
	{ "none", 0U, 0},
#define VCL_MET_MAC(l,U,t,b)	{ "vcl_"#l, b, VCL_MET_##U },
#include "tbl/vcl_returns.h"
	{ NULL, 0U, 0}
};

/*--------------------------------------------------------------------*/

void * v_matchproto_(TlAlloc)
TlAlloc(struct vcc *tl, unsigned len)
{
	void *p;

	(void)tl;
	p = calloc(1, len);
	assert(p != NULL);
	return (p);
}

char *
TlDup(struct vcc *tl, const char *s)
{
	char *p;

	p = TlAlloc(tl, strlen(s) + 1);
	AN(p);
	strcpy(p, s);
	return (p);
}

static int
TLWriteVSB(struct vcc *tl, const char *fn, const struct vsb *vsb,
    const char *what)
{
	int fo;
	int i;

	fo = open(fn, O_WRONLY|O_TRUNC|O_CREAT, 0600);
	if (fo < 0) {
		VSB_printf(tl->sb,
		    "Could not open %s file %s: %s\n",
		    what, fn, strerror(errno));
		return (-1);
	}
	i = VSB_tofile(fo, vsb);
	if (i) {
		VSB_printf(tl->sb,
		    "Could not write %s to %s: %s\n",
		    what, fn, strerror(errno));
	}
	closefd(&fo);
	return (i);
}

/*--------------------------------------------------------------------*/

struct proc *
vcc_NewProc(struct vcc *tl, struct symbol *sym)
{
	struct proc *p;

	ALLOC_OBJ(p, PROC_MAGIC);
	AN(p);
	VTAILQ_INIT(&p->calls);
	VTAILQ_INIT(&p->uses);
	VTAILQ_INIT(&p->priv_tasks);
	VTAILQ_INIT(&p->priv_tops);
	VTAILQ_INSERT_TAIL(&tl->procs, p, list);
	p->prologue = VSB_new_auto();
	AN(p->prologue);
	p->body = VSB_new_auto();
	AN(p->body);
	p->cname = VSB_new_auto();
	AN(p->cname);
	sym->proc = p;
	return (p);
}

static void
vcc_EmitProc(struct vcc *tl, struct proc *p)
{
	AZ(VSB_finish(p->cname));
	AZ(VSB_finish(p->prologue));
	AZ(VSB_finish(p->body));
	Fh(tl, 1, "vcl_func_f %s;\n", VSB_data(p->cname));
	Fc(tl, 1, "\nvoid v_matchproto_(vcl_func_f)\n");
	Fc(tl, 1, "%s(VRT_CTX)\n", VSB_data(p->cname));
	Fc(tl, 1, "{\n%s\n%s}\n", VSB_data(p->prologue), VSB_data(p->body));
	VSB_destroy(&p->body);
	VSB_destroy(&p->prologue);
	VSB_destroy(&p->cname);
}

/*--------------------------------------------------------------------*/

struct inifin *
New_IniFin(struct vcc *tl)
{
	struct inifin *p;

	ALLOC_OBJ(p, INIFIN_MAGIC);
	AN(p);
	p->ini = VSB_new_auto();
	AN(p->ini);
	p->fin = VSB_new_auto();
	AN(p->fin);
	p->final = VSB_new_auto();
	AN(p->final);
	p->event = VSB_new_auto();
	AN(p->event);
	p->n = ++tl->ninifin;
	VTAILQ_INSERT_TAIL(&tl->inifin, p, list);
	return (p);
}

/*--------------------------------------------------------------------
 * Printf output to the vsbs, possibly indented
 */

void
Fh(const struct vcc *tl, int indent, const char *fmt, ...)
{
	va_list ap;

	if (indent)
		VSB_printf(tl->fh, "%*.*s", tl->hindent, tl->hindent, "");
	va_start(ap, fmt);
	VSB_vprintf(tl->fh, fmt, ap);
	va_end(ap);
}

void
Fb(const struct vcc *tl, int indent, const char *fmt, ...)
{
	va_list ap;

	assert(tl->fb != NULL);
	if (indent)
		VSB_printf(tl->fb, "%*.*s", tl->indent, tl->indent, "");
	va_start(ap, fmt);
	VSB_vprintf(tl->fb, fmt, ap);
	va_end(ap);
}

void
Fc(const struct vcc *tl, int indent, const char *fmt, ...)
{
	va_list ap;

	if (indent)
		VSB_printf(tl->fc, "%*.*s", tl->indent, tl->indent, "");
	va_start(ap, fmt);
	VSB_vprintf(tl->fc, fmt, ap);
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
EncToken(struct vsb *sb, const struct token *t)
{

	assert(t->tok == CSTR);
	VSB_quote(sb, t->dec, -1, VSB_QUOTE_CSTR);
}

/*--------------------------------------------------------------------
 * Output the location/profiling table.  For each counted token, we
 * record source+line+charpos for the first character in the token.
 */

static void
EmitCoordinates(const struct vcc *tl, struct vsb *vsb)
{
	struct token *t;
	unsigned lin, pos;
	struct source *sp;
	const char *p;

	VSB_printf(vsb, "/* ---===### Source Code ###===---*/\n");

	VSB_printf(vsb, "\n#define VGC_NSRCS %u\n", tl->nsources);

	VSB_printf(vsb, "\nstatic const char *srcname[VGC_NSRCS] = {\n");
	VTAILQ_FOREACH(sp, &tl->sources, list) {
		VSB_printf(vsb, "\t");
		VSB_quote(vsb, sp->name, -1, VSB_QUOTE_CSTR);
		VSB_printf(vsb, ",\n");
	}
	VSB_printf(vsb, "};\n");

	VSB_printf(vsb, "\nstatic const char *srcbody[%u] = {\n", tl->nsources);
	VTAILQ_FOREACH(sp, &tl->sources, list) {
		VSB_printf(vsb, "    /* ");
		VSB_quote(vsb, sp->name, -1, VSB_QUOTE_CSTR);
		VSB_printf(vsb, " */\n");
		VSB_quote_pfx(vsb, "\t", sp->b, sp->e - sp->b, VSB_QUOTE_CSTR);
		VSB_printf(vsb, ",\n");
	}
	VSB_printf(vsb, "};\n\n");

	VSB_printf(vsb, "/* ---===### Location Counters ###===---*/\n");

	VSB_printf(vsb, "\n#define VGC_NREFS %u\n\n", tl->cnt + 1);

	VSB_printf(vsb, "static const struct vpi_ref VGC_ref[VGC_NREFS] = {\n");
	lin = 1;
	pos = 0;
	sp = 0;
	p = NULL;
	VTAILQ_FOREACH(t, &tl->tokens, list) {
		if (t->cnt == 0)
			continue;
		assert(t->src != NULL);
		if (t->src != sp) {
			lin = 1;
			pos = 0;
			sp = t->src;
			p = sp->b;
		}
		assert(sp != NULL);
		assert(p != NULL);
		for (;p < t->b; p++) {
			if (*p == '\n') {
				lin++;
				pos = 0;
			} else if (*p == '\t') {
				pos &= ~7;
				pos += 8;
			} else
				pos++;

		}
		VSB_printf(vsb, "  [%3u] = { %u, %8tu, %4u, %3u, ",
		    t->cnt, sp->idx, t->b - sp->b, lin, pos + 1);
		if (t->tok == CSRC)
			VSB_printf(vsb, " \"C{\"},\n");
		else
			VSB_printf(vsb, " \"%.*s\" },\n", PF(t));
	}
	VSB_printf(vsb, "};\n\n");
}

/*--------------------------------------------------------------------
 * Init/Fini/Event
 *
 * We call DISCARD and COLD events in the opposite order of LOAD and
 * WARM.
 */

static void
EmitInitFini(const struct vcc *tl)
{
	struct inifin *p, *q = NULL;
	unsigned has_event = 0;
	struct symbol *sy;

	Fh(tl, 0, "\n");
	Fh(tl, 0, "static unsigned vgc_inistep;\n");
	Fh(tl, 0, "static unsigned vgc_warmupstep;\n");

	/*
	 * LOAD
	 */
	Fc(tl, 0, "\nstatic int\nVGC_Load(VRT_CTX)\n{\n\n");
	Fc(tl, 0, "\tvgc_inistep = 0;\n");
	Fc(tl, 0, "\tsize_t ndirector = %dUL;\n", tl->ndirector);
	Fc(tl, 0, "\n");
	VTAILQ_FOREACH(p, &tl->inifin, list) {
		AZ(VSB_finish(p->ini));
		assert(p->n > 0);
		if (VSB_len(p->ini))
			Fc(tl, 0, "\t/* %u */\n%s\n", p->n, VSB_data(p->ini));
		if (p->ignore_errors == 0) {
			Fc(tl, 0, "\tif (*ctx->handling == VCL_RET_FAIL)\n");
			Fc(tl, 0, "\t\treturn(1);\n");
		}
		Fc(tl, 0, "\tvgc_inistep = %u;\n\n", p->n);
		VSB_destroy(&p->ini);

		AZ(VSB_finish(p->event));
		if (VSB_len(p->event))
			has_event = 1;
	}

	/* Handle failures from vcl_init */
	Fc(tl, 0, "\n");
	Fc(tl, 0, "\tif (*ctx->handling != VCL_RET_OK)\n");
	Fc(tl, 0, "\t\treturn(1);\n");

	VTAILQ_FOREACH(sy, &tl->sym_objects, sideways) {
		Fc(tl, 0, "\tif (!%s) {\n", sy->rname);
		Fc(tl, 0, "\t\t*ctx->handling = 0;\n");
		Fc(tl, 0, "\t\tVRT_fail(ctx, "
		    "\"Object %s not initialized\");\n" , sy->name);
		Fc(tl, 0, "\t\treturn(1);\n");
		Fc(tl, 0, "\t}\n");
	}

	Fc(tl, 0, "\treturn(0);\n");
	Fc(tl, 0, "}\n");

	/*
	 * DISCARD
	 */
	Fc(tl, 0, "\nstatic int\nVGC_Discard(VRT_CTX)\n{\n\n");

	Fc(tl, 0, "\tswitch (vgc_inistep) {\n");
	VTAILQ_FOREACH_REVERSE(p, &tl->inifin, inifinhead, list) {
		AZ(VSB_finish(p->fin));
		if (q)
			assert(q->n > p->n);
		q = p;
		Fc(tl, 0, "\t\tcase %u:\n", p->n);
		if (VSB_len(p->fin))
			Fc(tl, 0, "\t%s\n", VSB_data(p->fin));
		Fc(tl, 0, "\t\t\t/* FALLTHROUGH */\n");
		VSB_destroy(&p->fin);
	}
	Fc(tl, 0, "\t\tdefault:\n\t\t\tbreak;\n");
	Fc(tl, 0, "\t}\n\n");
	Fc(tl, 0, "\tswitch (vgc_inistep) {\n");
	VTAILQ_FOREACH_REVERSE(p, &tl->inifin, inifinhead, list) {
		AZ(VSB_finish(p->final));
		Fc(tl, 0, "\t\tcase %u:\n", p->n);
		if (VSB_len(p->final))
			Fc(tl, 0, "\t%s\n", VSB_data(p->final));
		Fc(tl, 0, "\t\t\t/* FALLTHROUGH */\n");
		VSB_destroy(&p->final);
	}
	Fc(tl, 0, "\t\tdefault:\n\t\t\tbreak;\n");
	Fc(tl, 0, "\t}\n\n");

	Fc(tl, 0, "\treturn (0);\n");
	Fc(tl, 0, "}\n");

	if (has_event) {
		/*
		 * WARM
		 */
		Fc(tl, 0, "\nstatic int\n");
		Fc(tl, 0, "VGC_Warmup(VRT_CTX, enum vcl_event_e ev)\n{\n\n");

		Fc(tl, 0, "\tvgc_warmupstep = 0;\n\n");
		VTAILQ_FOREACH(p, &tl->inifin, list) {
			assert(p->n > 0);
			if (VSB_len(p->event)) {
				Fc(tl, 0, "\t/* %u */\n", p->n);
				Fc(tl, 0, "\tif (%s)\n", VSB_data(p->event));
				Fc(tl, 0, "\t\treturn (1);\n");
				Fc(tl, 0, "\tvgc_warmupstep = %u;\n\n", p->n);
			}
		}

		Fc(tl, 0, "\treturn (0);\n");
		Fc(tl, 0, "}\n");

		/*
		 * COLD
		 */
		Fc(tl, 0, "\nstatic int\n");
		Fc(tl, 0, "VGC_Cooldown(VRT_CTX, enum vcl_event_e ev)\n{\n");
		Fc(tl, 0, "\tint retval = 0;\n\n");

		VTAILQ_FOREACH_REVERSE(p, &tl->inifin, inifinhead, list) {
			if (VSB_len(p->event)) {
				Fc(tl, 0, "\t/* %u */\n", p->n);
				Fc(tl, 0,
				    "\tif (vgc_warmupstep >= %u &&\n", p->n);
				Fc(tl, 0,
				    "\t    %s != 0)\n", VSB_data(p->event));
				Fc(tl, 0, "\t\tretval = 1;\n\n");
			}
			VSB_destroy(&p->event);
		}

		Fc(tl, 0, "\treturn (retval);\n");
		Fc(tl, 0, "}\n");
	}

	/*
	 * EVENTS
	 */
	Fc(tl, 0, "\nstatic int\n");
	Fc(tl, 0, "VGC_Event(VRT_CTX, enum vcl_event_e ev)\n");
	Fc(tl, 0, "{\n");
	Fc(tl, 0, "\tif (ev == VCL_EVENT_LOAD)\n");
	Fc(tl, 0, "\t\treturn (VGC_Load(ctx));\n");
	if (has_event) {
		Fc(tl, 0, "\tif (ev == VCL_EVENT_WARM)\n");
		Fc(tl, 0, "\t\treturn (VGC_Warmup(ctx, ev));\n");
		Fc(tl, 0, "\tif (ev == VCL_EVENT_COLD)\n");
		Fc(tl, 0, "\t\treturn (VGC_Cooldown(ctx, ev));\n");
	}
	Fc(tl, 0, "\tif (ev == VCL_EVENT_DISCARD)\n");
	Fc(tl, 0, "\t\treturn (VGC_Discard(ctx));\n");
	Fc(tl, 0, "\n");
	if (!has_event)
		Fc(tl, 0, "\t(void)vgc_warmupstep;\n");
	Fc(tl, 0, "\treturn (%d);\n", has_event ? 1 : 0);
	Fc(tl, 0, "}\n");
}

/*--------------------------------------------------------------------*/

static void
EmitStruct(const struct vcc *tl)
{
	Fc(tl, 0, "\nconst struct VCL_conf VCL_conf = {\n");
	Fc(tl, 0, "\t.magic = VCL_CONF_MAGIC,\n");
	Fc(tl, 0, "\t.syntax = %u,\n", tl->syntax);
	Fc(tl, 0, "\t.event_vcl = VGC_Event,\n");
	Fc(tl, 0, "\t.default_director = &%s,\n", tl->default_director);
	if (tl->default_probe != NULL)
		Fc(tl, 0, "\t.default_probe = %s,\n", tl->default_probe);
	Fc(tl, 0, "\t.ref = VGC_ref,\n");
	Fc(tl, 0, "\t.nref = VGC_NREFS,\n");
	Fc(tl, 0, "\t.nsrc = VGC_NSRCS,\n");
	Fc(tl, 0, "\t.srcname = srcname,\n");
	Fc(tl, 0, "\t.srcbody = srcbody,\n");
	Fc(tl, 0, "\t.nvmod = %u,\n", tl->vmod_count);
#define VCL_MET_MAC(l,u,t,b) \
	Fc(tl, 0, "\t." #l "_func = VGC_function_vcl_" #l ",\n");
#include "tbl/vcl_returns.h"
	Fc(tl, 0, "};\n");
}

/*--------------------------------------------------------------------*/

static struct source *
vcc_new_source(const char *b, const char *e, const char *name)
{
	struct source *sp;

	if (e == NULL)
		e = strchr(b, '\0');
	sp = calloc(1, sizeof *sp);
	assert(sp != NULL);
	sp->name = strdup(name);
	AN(sp->name);
	sp->b = b;
	sp->e = e;
	return (sp);
}

/*--------------------------------------------------------------------*/

static struct source *
vcc_file_source(const struct vcc *tl, const char *fn)
{
	char *f, *fnp;
	struct source *sp;

	if (!tl->unsafe_path && strchr(fn, '/') != NULL) {
		VSB_printf(tl->sb, "VCL filename '%s' is unsafe.\n", fn);
		return (NULL);
	}
	f = NULL;
	if (VFIL_searchpath(tl->vcl_path, NULL, &f, fn, &fnp) || f == NULL) {
		VSB_printf(tl->sb, "Cannot read file '%s' (%s)\n",
		    fnp != NULL ? fnp : fn, strerror(errno));
		free(fnp);
		return (NULL);
	}
	sp = vcc_new_source(f, NULL, fnp);
	free(fnp);
	sp->freeit = f;
	return (sp);
}

/*--------------------------------------------------------------------*/

static void
vcc_resolve_includes(struct vcc *tl)
{
	struct token *t, *t1, *t2;
	struct source *sp;
	struct vsb *vsb;
	const char *p;

	VTAILQ_FOREACH(t, &tl->tokens, list) {
		if (t->tok != ID || !vcc_IdIs(t, "include"))
			continue;

		t1 = VTAILQ_NEXT(t, list);
		AN(t1);			/* There's always an EOI */
		if (t1->tok != CSTR) {
			VSB_printf(tl->sb,
			    "include not followed by string constant.\n");
			vcc_ErrWhere(tl, t1);
			return;
		}
		t2 = VTAILQ_NEXT(t1, list);
		AN(t2);			/* There's always an EOI */

		if (t2->tok != ';') {
			VSB_printf(tl->sb,
			    "include <string> not followed by semicolon.\n");
			vcc_ErrWhere(tl, t1);
			return;
		}

		if (t1->dec[0] == '.' && t1->dec[1] == '/') {
			/*
			 * Nested include filenames, starting with "./" are
			 * resolved relative to the VCL file which contains
			 * the include directive.
			 */
			if (t1->src->name[0] != '/') {
				VSB_printf(tl->sb,
				    "include \"./xxxxx\"; needs absolute "
				    "filename of including file.\n");
				vcc_ErrWhere(tl, t1);
				return;
			}
			vsb = VSB_new_auto();
			AN(vsb);
			p = strrchr(t1->src->name, '/');
			AN(p);
			VSB_bcat(vsb, t1->src->name, p - t1->src->name);
			VSB_cat(vsb, t1->dec + 1);
			AZ(VSB_finish(vsb));
			sp = vcc_file_source(tl, VSB_data(vsb));
			VSB_destroy(&vsb);
		} else {
			sp = vcc_file_source(tl, t1->dec);
		}
		if (sp == NULL) {
			vcc_ErrWhere(tl, t1);
			return;
		}
		VTAILQ_INSERT_TAIL(&tl->sources, sp, list);
		sp->idx = tl->nsources++;
		tl->t = t2;
		vcc_Lexer(tl, sp);

		VTAILQ_REMOVE(&tl->tokens, t, list);
		VTAILQ_REMOVE(&tl->tokens, t1, list);
		VTAILQ_REMOVE(&tl->tokens, t2, list);
		if (!tl->err)
			vcc_resolve_includes(tl);
		return;
	}
}

/*--------------------------------------------------------------------
 * Compile the VCL code from the given source and return the C-source
 */

static struct vsb *
vcc_CompileSource(struct vcc *tl, struct source *sp, const char *jfile)
{
	struct proc *p;
	struct vsb *vsb;
	struct inifin *ifp;

	Fh(tl, 0, "/* ---===### VCC generated .h code ###===---*/\n");
	Fc(tl, 0, "\n/* ---===### VCC generated .c code ###===---*/\n");

	Fc(tl, 0, "\n#define END_ if (*ctx->handling) return\n");

	vcc_Parse_Init(tl);

	vcc_Expr_Init(tl);

	vcc_Action_Init(tl);

	vcc_Backend_Init(tl);

	vcc_Var_Init(tl);

	Fh(tl, 0, "\nextern const struct VCL_conf VCL_conf;\n");

	/* Register and lex the main source */
	VTAILQ_INSERT_TAIL(&tl->sources, sp, list);
	sp->idx = tl->nsources++;
	vcc_Lexer(tl, sp);
	if (tl->err)
		return (NULL);

	/* Register and lex the builtin VCL */
	sp = vcc_new_source(tl->builtin_vcl, NULL, "Builtin");
	assert(sp != NULL);
	VTAILQ_INSERT_TAIL(&tl->sources, sp, list);
	sp->idx = tl->nsources++;
	vcc_Lexer(tl, sp);
	if (tl->err)
		return (NULL);

	/* Add "END OF INPUT" token */
	vcc_AddToken(tl, EOI, sp->e, sp->e);
	if (tl->err)
		return (NULL);

	/* Expand and lex any includes in the token string */
	vcc_resolve_includes(tl);
	if (tl->err)
		return (NULL);

	/* Parse the token string */
	tl->t = VTAILQ_FIRST(&tl->tokens);
	vcc_Parse(tl);
	if (tl->err)
		return (NULL);

	/* Check if we have any backends at all */
	if (tl->default_director == NULL) {
		VSB_printf(tl->sb,
		    "No backends or directors found in VCL program, "
		    "at least one is necessary.\n");
		tl->err = 1;
		return (NULL);
	}

	/* Check for orphans */
	if (vcc_CheckReferences(tl))
		return (NULL);

	/* Check that all action returns are legal */
	if (vcc_CheckAction(tl) || tl->err)
		return (NULL);

	/* Check that all variable uses are legal */
	if (vcc_CheckUses(tl) || tl->err)
		return (NULL);

	/* Tie vcl_init/fini in */
	ifp = New_IniFin(tl);
	VSB_printf(ifp->ini, "\tVGC_function_vcl_init(ctx);\n");
	/*
	 * Because the failure could be half way into vcl_init{} so vcl_fini{}
	 * must always be called, also on failure.
	 */
	ifp->ignore_errors = 1;
	VSB_printf(ifp->fin, "\t\tVGC_function_vcl_fini(ctx);");

	/* Emit method functions */
	Fh(tl, 1, "\n");
	VTAILQ_FOREACH(p, &tl->procs, list)
		if (p->method == NULL)
			vcc_EmitProc(tl, p);
	VTAILQ_FOREACH(p, &tl->procs, list)
		if (p->method != NULL)
			vcc_EmitProc(tl, p);

	EmitInitFini(tl);

	EmitStruct(tl);

	VCC_XrefTable(tl);

	VSB_printf(tl->symtab, "\n]\n");
	AZ(VSB_finish(tl->symtab));
	if (TLWriteVSB(tl, jfile, tl->symtab, "Symbol table"))
		return (NULL);

	/* Combine it all */

	vsb = VSB_new_auto();
	AN(vsb);

	vcl_output_lang_h(vsb);

	EmitCoordinates(tl, vsb);

	AZ(VSB_finish(tl->fh));
	VSB_cat(vsb, VSB_data(tl->fh));

	AZ(VSB_finish(tl->fc));
	VSB_cat(vsb, VSB_data(tl->fc));

	AZ(VSB_finish(vsb));
	return (vsb);
}

/*--------------------------------------------------------------------
 * Report the range of VCL language we support
 */
void
VCC_VCL_Range(unsigned *lo, unsigned *hi)
{

	AN(lo);
	*lo = VCL_LOW;
	AN(hi);
	*hi = VCL_HIGH;
}

/*--------------------------------------------------------------------
 * Compile the VCL code in the argument.  Error messages, if any are
 * formatted into the vsb.
 */

int
VCC_Compile(struct vcc *tl, struct vsb **sb,
    const char *vclsrc, const char *vclsrcfile,
    const char *ofile, const char *jfile)
{
	struct source *sp;
	struct vsb *r = NULL;
	int retval = 0;

	CHECK_OBJ_NOTNULL(tl, VCC_MAGIC);
	AN(sb);
	AN(vclsrcfile);
	AN(ofile);
	AN(jfile);
	if (vclsrc != NULL)
		sp = vcc_new_source(vclsrc, NULL, vclsrcfile);
	else
		sp = vcc_file_source(tl, vclsrcfile);

	if (sp != NULL)
		r = vcc_CompileSource(tl, sp, jfile);

	if (r != NULL) {
		retval = TLWriteVSB(tl, ofile, r, "C-source");
		VSB_destroy(&r);
	} else {
		retval = -1;
	}
	AZ(VSB_finish(tl->sb));
	*sb = tl->sb;
	return (retval);
}

/*--------------------------------------------------------------------
 * Allocate a compiler instance
 */

struct vcc *
VCC_New(void)
{
	struct vcc *tl;
	struct symbol *sym;
	struct proc *p;
	int i;

	ALLOC_OBJ(tl, VCC_MAGIC);
	AN(tl);
	VTAILQ_INIT(&tl->inifin);
	VTAILQ_INIT(&tl->tokens);
	VTAILQ_INIT(&tl->sources);
	VTAILQ_INIT(&tl->procs);
	VTAILQ_INIT(&tl->sym_objects);
	VTAILQ_INIT(&tl->sym_vmods);

	tl->nsources = 0;

	tl->symtab = VSB_new_auto();
	assert(tl->symtab != NULL);
	VSB_printf(tl->symtab, "[\n    {\"version\": 0}");

	tl->fc = VSB_new_auto();
	assert(tl->fc != NULL);

	tl->fh = VSB_new_auto();
	assert(tl->fh != NULL);

	for (i = 1; i < VCL_MET_MAX; i++) {
		sym = VCC_MkSym(tl, method_tab[i].name,
		    SYM_SUB, VCL_LOW, VCL_HIGH);
		p = vcc_NewProc(tl, sym);
		p->method = &method_tab[i];
		VSB_printf(p->cname, "VGC_function_%s", p->method->name);
	}
	tl->sb = VSB_new_auto();
	AN(tl->sb);
	return (tl);
}

/*--------------------------------------------------------------------
 * Configure builtin VCL source code
 */

void
VCC_Builtin_VCL(struct vcc *vcc, const char *str)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	REPLACE(vcc->builtin_vcl, str);
}

/*--------------------------------------------------------------------
 * Configure default VCL source path
 */

void
VCC_VCL_path(struct vcc *vcc, const char *str)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	VFIL_setpath(&vcc->vcl_path, str);
}

/*--------------------------------------------------------------------
 * Configure default VMOD path
 */

void
VCC_VMOD_path(struct vcc *vcc, const char *str)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	VFIL_setpath(&vcc->vmod_path, str);
}

/*--------------------------------------------------------------------
 * Configure settings
 */

void
VCC_Err_Unref(struct vcc *vcc, unsigned u)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	vcc->err_unref = u;
}

void
VCC_Allow_InlineC(struct vcc *vcc, unsigned u)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	vcc->allow_inline_c = u;
}

void
VCC_Unsafe_Path(struct vcc *vcc, unsigned u)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	vcc->unsafe_path = u;
}

/*--------------------------------------------------------------------
 * Configure settings
 */

static void
vcc_predef_vcl(struct vcc *vcc, const char *name)
{
	struct symbol *sym;

	sym = VCC_MkSym(vcc, name, SYM_VCL, VCL_LOW, VCL_HIGH);
	AN(sym);
	sym->type = VCL;
	sym->r_methods = VCL_MET_RECV;
}

void
VCC_Predef(struct vcc *vcc, const char *type, const char *name)
{

	CHECK_OBJ_NOTNULL(vcc, VCC_MAGIC);
	if (!strcmp(type, "VCL_STEVEDORE"))
		vcc_stevedore(vcc, name);
	else if (!strcmp(type, "VCL_VCL"))
		vcc_predef_vcl(vcc, name);
	else
		WRONG("Unknown VCC predef type");
}
