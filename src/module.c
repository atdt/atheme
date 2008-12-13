/*
 * atheme-services: A collection of minimalist IRC services
 * module.c: Module management.
 *
 * Copyright (c) 2005-2007 Atheme Project (http://www.atheme.org)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "atheme.h"
#include "linker.h"

#include <dlfcn.h>

static BlockHeap *module_heap;
list_t modules;

module_t *modtarget = NULL;

void modules_init(void)
{
	module_heap = BlockHeapCreate(sizeof(module_t), 256);

	if (!module_heap)
	{
		slog(LG_ERROR, "modules_init(): block allocator failed.");
		exit(EXIT_FAILURE);
	}
}

/*
 * module_load()
 *
 * inputs:
 *       a literal filename for a module to load.
 *
 * outputs:
 *       the respective module_t object of the module.
 *
 * side effects:
 *       a module is loaded and necessary initialization code is run.
 */
module_t *module_load(const char *filespec)
{
	node_t *n;
	module_t *m;
	v2_moduleheader_t *h;
	void *handle = NULL;
#ifdef HAVE_DLINFO
	struct link_map *map;
#endif

	if ((m = module_find(filespec)))
	{
		slog(LG_INFO, "module_load(): module %s is already loaded [at 0x%lx]", filespec, (unsigned long)m->address);
		return NULL;
	}

	handle = linker_open_ext(filespec);

	if (!handle)
	{
		char *errp = sstrdup(dlerror());
		slog(LG_ERROR, "module_load(): error: %s", errp);
		if (me.connected)
			snoop(_("MODLOAD:ERROR: loading module \2%s\2: %s"), filespec, errp);
		free(errp);
		return NULL;
	}

	h = (v2_moduleheader_t *) linker_getsym(handle, "_header");

	if (h == NULL || h->atheme_mod != MAPI_ATHEME_MAGIC)
	{
		slog(LG_DEBUG, "module_load(): %s: Attempted to load an incompatible module. Aborting.", filespec);

		if (me.connected)
			snoop(_("MODLOAD:ERROR: Module \2%s\2 is not a valid atheme module."), filespec);

		linker_close(handle);
		return NULL;
	}

	if (h->abi_ver != MAPI_ATHEME_V2)
	{
		slog(LG_ERROR, "module_load(): %s: MAPI version mismatch (%u != %u), please recompile.", filespec, h->abi_ver, MAPI_ATHEME_V2);

		if (me.connected)
			snoop(_("MODLOAD:ERROR: Module \2%s\2 has wrong MAPI version (%u != %u), please recompile it."), filespec, h->abi_ver, MAPI_ATHEME_V2);

		linker_close(handle);
		return NULL;
	}

	if (h->abi_rev != CURRENT_ABI_REVISION)
	{
		slog(LG_ERROR, "module_load(): %s: ABI revision mismatch (%u != %u), please recompile.", filespec, h->abi_rev, CURRENT_ABI_REVISION);

		if (me.connected)
			snoop(_("MODLOAD:ERROR: Module \2%s\2 has wrong ABI revision (%u != %u), please recompile it."), filespec, h->abi_rev, CURRENT_ABI_REVISION);

		linker_close(handle);
		return NULL;
	}

	if (module_find_published(h->name))
	{
		slog(LG_DEBUG, "module_load(): %s: Published name %s already exists.", filespec, h->name);

		if (me.connected)
			snoop(_("MODLOAD:ERROR: Module \2%s\2 already exists while loading \2%s\2."), h->name, filespec);

		linker_close(handle);
		return NULL;
	}

	m = BlockHeapAlloc(module_heap);

	strlcpy(m->modpath, filespec, BUFSIZE);
	m->handle = handle;
	m->mflags = MODTYPE_STANDARD;
	m->header = h;

#ifdef HAVE_DLINFO
	dlinfo(handle, RTLD_DI_LINKMAP, &map);
	if (map != NULL)
		m->address = (void *) map->l_addr;
	else
		m->address = handle;
#else
	/* best we can do here without dlinfo() --nenolod */
	m->address = handle;
#endif

	/* set the module target for module dependencies */
	modtarget = m;

	if (h->modinit)
		h->modinit(m);

	/* we won't be loading symbols outside the init code */
	modtarget = NULL;

	if (m->mflags & MODTYPE_FAIL)
	{
		slog(LG_ERROR, "module_load(): module %s init failed", filespec);
		if (me.connected)
			snoop(_("MODLOAD:ERROR: Init failed while loading module \2%s\2"), filespec);
		module_unload(m);
		return NULL;
	}

	n = node_create();
	node_add(m, n, &modules);

	slog(LG_DEBUG, "module_load(): loaded %s [at 0x%lx; MAPI version %d]", h->name, (unsigned long)m->address, h->abi_ver);

	if (me.connected && !cold_start)
	{
		wallops(_("Module %s loaded [at 0x%lx; MAPI version %d]"), h->name, (unsigned long)m->address, h->abi_ver);
		snoop(_("MODLOAD: \2%s\2 [at 0x%lx; MAPI version %d]"), h->name, (unsigned long)m->address, h->abi_ver);
	}

	return m;
}

/*
 * module_load_dir()
 *
 * inputs:
 *       a directory containing modules to load.
 *
 * outputs:
 *       none
 *
 * side effects:
 *       qualifying modules are passed to module_load().
 */
void module_load_dir(const char *dirspec)
{
	DIR *module_dir = NULL;
	struct dirent *ldirent = NULL;
	char module_filename[4096];

	module_dir = opendir(dirspec);

	if (!module_dir)
	{
		slog(LG_ERROR, "module_load_dir(): %s: %s", dirspec, strerror(errno));
		return;
	}

	while ((ldirent = readdir(module_dir)) != NULL)
	{
		if (!strstr(ldirent->d_name, ".so"))
			continue;

		snprintf(module_filename, sizeof(module_filename), "%s/%s", dirspec, ldirent->d_name);
		module_load(module_filename);
	}

	closedir(module_dir);
}

/*
 * module_load_dir_match()
 *
 * inputs:
 *       a directory containing modules to load, and a pattern to match against
 *       to determine whether or not a module qualifies for loading.
 *
 * outputs:
 *       none
 *
 * side effects:
 *       qualifying modules are passed to module_load().
 */
void module_load_dir_match(const char *dirspec, const char *pattern)
{
	DIR *module_dir = NULL;
	struct dirent *ldirent = NULL;
	char module_filename[4096];

	module_dir = opendir(dirspec);

	if (!module_dir)
	{
		slog(LG_ERROR, "module_load_dir(): %s: %s", dirspec, strerror(errno));
		return;
	}

	while ((ldirent = readdir(module_dir)) != NULL)
	{
		if (!strstr(ldirent->d_name, ".so") && match(pattern, ldirent->d_name))
			continue;

		snprintf(module_filename, sizeof(module_filename), "%s/%s", dirspec, ldirent->d_name);
		module_load(module_filename);
	}

	closedir(module_dir);
}

/*
 * module_unload()
 *
 * inputs:
 *       a module object to unload.
 *
 * outputs:
 *       none
 *
 * side effects:
 *       a module is unloaded and neccessary deinitalization code is run.
 */
void module_unload(module_t * m)
{
	node_t *n, *tn;

	if (!m)
		return;

	/* unload modules which depend on us */
	LIST_FOREACH_SAFE(n, tn, m->dephost.head)
		module_unload((module_t *) n->data);

	/* let modules that we depend on know that we no longer exist */
	LIST_FOREACH_SAFE(n, tn, m->deplist.head)
	{
		module_t *hm = (module_t *) n->data;
		node_t *hn = node_find(m, &hm->dephost);

		node_del(hn, &hm->dephost);		
		node_free(hn);
		node_del(n, &m->deplist);
		node_free(n);
	}

	n = node_find(m, &modules);
	if (n != NULL)
	{
		slog(LG_INFO, "module_unload(): unloaded %s", m->header->name);
		if (me.connected)
		{
			wallops(_("Module %s unloaded."), m->header->name);
			snoop("MODUNLOAD: \2%s\2", m->header->name);
		}

		if (m->header->deinit)
			m->header->deinit();
		node_del(n, &modules);
		node_free(n);
	}
	/* else unloaded in embryonic state */
	linker_close(m->handle);
	BlockHeapFree(module_heap, m);
}

/*
 * module_locate_symbol()
 *
 * inputs:
 *       a name of a module and a symbol to look for inside it.
 *
 * outputs:
 *       the pointer to the module symbol, else NULL if not found.
 *
 * side effects:
 *       none
 */
void *module_locate_symbol(const char *modname, const char *sym)
{
	module_t *m;
	void *symptr;

	if (!(m = module_find_published(modname)))
	{
		slog(LG_ERROR, "module_locate_symbol(): %s is not loaded.", modname);
		return NULL;
	}

	if (modtarget != NULL && !node_find(m, &modtarget->deplist))
	{
		slog(LG_DEBUG, "module_locate_symbol(): %s added as a dependency for %s (symbol: %s)",
			m->header->name, modtarget->header->name, sym);
		node_add(m, node_create(), &modtarget->deplist);
		node_add(modtarget, node_create(), &m->dephost);
	}

	symptr = linker_getsym(m->handle, sym);

	if (symptr == NULL)
		slog(LG_ERROR, "module_locate_symbol(): could not find symbol %s in module %s.", sym, modname);
	return symptr;
}

/*
 * module_find()
 *
 * inputs:
 *       a name of a module to locate the object for.
 *
 * outputs:
 *       the module object if the module is located, else NULL.
 *
 * side effects:
 *       none
 */
module_t *module_find(const char *name)
{
	node_t *n;

	LIST_FOREACH(n, modules.head)
	{
		module_t *m = n->data;

		if (!strcasecmp(m->modpath, name))
			return m;
	}

	return NULL;
}

/*
 * module_find_published()
 *
 * inputs:
 *       a published (in _header) name of a module to locate the object for.
 *
 * outputs:
 *       the module object if the module is located, else NULL.
 *
 * side effects:
 *       none
 */
module_t *module_find_published(const char *name)
{
	node_t *n;

	LIST_FOREACH(n, modules.head)
	{
		module_t *m = n->data;

		if (!strcasecmp(m->header->name, name))
			return m;
	}

	return NULL;
}

/*
 * module_request()
 *
 * inputs:
 *       a published name of a module to load.
 *
 * outputs:
 *       true: if the module was loaded, or is already present.
 *
 * side effects:
 *       a module might be loaded.
 */
bool module_request(const char *name)
{
	module_t *m;
	char path[BUFSIZE];

	if ((m = module_find_published(name)) != NULL)
		return true;

	snprintf(path, BUFSIZE, "%s/modules/%s", MODDIR, name);
	if ((m = module_load(path)) == NULL)
		return false;

	return true;
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
