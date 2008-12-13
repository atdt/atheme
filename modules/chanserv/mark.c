/*
 * Copyright (c) 2005 William Pitcock
 * Rights to this code are as documented in doc/LICENSE.
 *
 * Marking for channels.
 *
 * $Id: mark.c 7895 2007-03-06 02:40:03Z pippijn $
 */

#include "atheme.h"

DECLARE_MODULE_V1
(
	"chanserv/mark", false, _modinit, _moddeinit,
	"$Id: mark.c 7895 2007-03-06 02:40:03Z pippijn $",
	"Atheme Development Group <http://www.atheme.org>"
);

static void cs_cmd_mark(sourceinfo_t *si, int parc, char *parv[]);

command_t cs_mark = { "MARK", N_("Adds a note to a channel."),
			PRIV_MARK, 3, cs_cmd_mark };

list_t *cs_cmdtree;
list_t *cs_helptree;

void _modinit(module_t *m)
{
	MODULE_USE_SYMBOL(cs_cmdtree, "chanserv/main", "cs_cmdtree");
	MODULE_USE_SYMBOL(cs_helptree, "chanserv/main", "cs_helptree");

	command_add(&cs_mark, cs_cmdtree);
	help_addentry(cs_helptree, "MARK", "help/cservice/mark", NULL);
}

void _moddeinit()
{
	command_delete(&cs_mark, cs_cmdtree);
	help_delentry(cs_helptree, "MARK");
}

static void cs_cmd_mark(sourceinfo_t *si, int parc, char *parv[])
{
	char *target = parv[0];
	char *action = parv[1];
	char *info = parv[2];
	mychan_t *mc;

	if (!target || !action)
	{
		command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "MARK");
		command_fail(si, fault_needmoreparams, _("Usage: MARK <#channel> <ON|OFF> [note]"));
		return;
	}

	if (target[0] != '#')
	{
		command_fail(si, fault_badparams, STR_INVALID_PARAMS, "MARK");
		return;
	}

	if (!(mc = mychan_find(target)))
	{
		command_fail(si, fault_nosuch_target, _("Channel \2%s\2 is not registered."), target);
		return;
	}
	
	if (!strcasecmp(action, "ON"))
	{
		if (!info)
		{
			command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "MARK");
			command_fail(si, fault_needmoreparams, _("Usage: MARK <#channel> ON <note>"));
			return;
		}

		if (metadata_find(mc, "private:mark:setter"))
		{
			command_fail(si, fault_nochange, _("\2%s\2 is already marked."), target);
			return;
		}

		metadata_add(mc, "private:mark:setter", get_oper_name(si));
		metadata_add(mc, "private:mark:reason", info);
		metadata_add(mc, "private:mark:timestamp", itoa(CURRTIME));

		wallops("%s marked the channel \2%s\2.", get_oper_name(si), target);
		snoop("MARK:ON: \2%s\2 by \2%s\2 for \2%s\2", target, get_oper_name(si), info);
		logcommand(si, CMDLOG_ADMIN, "%s MARK ON", mc->name);
		command_success_nodata(si, _("\2%s\2 is now marked."), target);
	}
	else if (!strcasecmp(action, "OFF"))
	{
		if (!metadata_find(mc, "private:mark:setter"))
		{
			command_fail(si, fault_nochange, _("\2%s\2 is not marked."), target);
			return;
		}

		metadata_delete(mc, "private:mark:setter");
		metadata_delete(mc, "private:mark:reason");
		metadata_delete(mc, "private:mark:timestamp");

		wallops("%s unmarked the channel \2%s\2.", get_oper_name(si), target);
		snoop("MARK:OFF: \2%s\2 by \2%s\2", target, get_oper_name(si));
		logcommand(si, CMDLOG_ADMIN, "%s MARK OFF", mc->name);
		command_success_nodata(si, _("\2%s\2 is now unmarked."), target);
	}
	else
	{
		command_fail(si, fault_badparams, STR_INVALID_PARAMS, "MARK");
		command_fail(si, fault_badparams, _("Usage: MARK <#channel> <ON|OFF> [note]"));
	}
}

/* vim:cinoptions=>s,e0,n0,f0,{0,}0,^0,=s,ps,t0,c3,+s,(2s,us,)20,*30,gs,hs
 * vim:ts=8
 * vim:sw=8
 * vim:noexpandtab
 */
