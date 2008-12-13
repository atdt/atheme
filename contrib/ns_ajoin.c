/*
 * Copyright (c) 2007 Jilles Tjoelker
 * Copyright (c) 2008 Robin Burchell
 * Rights to this code are as documented in doc/LICENSE.
 *
 * Services-side autojoin using SVSJOIN
 *
 * $Id$
 */

#include "atheme.h"
#include "uplink.h"

DECLARE_MODULE_V1
(
	"nickserv/ajoin", false, _modinit, _moddeinit,
	"$Id$",
	"Atheme Development Group <http://www.atheme.org>"
);

list_t *ns_cmdtree;

static void ajoin_on_identify(void *vptr);

static void ns_cmd_ajoin(sourceinfo_t *si, int parc, char *parv[])
{
	char buf[512];
	char *chan;
	metadata_t *md;

	if (!parv[0])
	{
		command_fail(si, fault_badparams, STR_INSUFFICIENT_PARAMS, "AJOIN");
		command_fail(si, fault_badparams, "Syntax: AJOIN <list|add|del> [#channel]");
		return;
	}

	if (!si->smu)
	{
		command_fail(si, fault_badparams, "You are not logged in.");
		return;
	}

	if (!strcasecmp(parv[0], "LIST"))
	{
		command_success_nodata(si, "\2AJOIN LIST\2:");
		if ((md = metadata_find(si->smu, "private:autojoin")))
		{
			strlcpy(buf, md->value, sizeof buf);

			chan = strtok(buf, ",");
			while (chan != NULL)
			{
				command_success_nodata(si, "%s", chan);
				chan = strtok(NULL, ",");
			}
		}
		command_success_nodata(si, "End of \2AJOIN LIST\2");
	}
	else if (!strcasecmp(parv[0], "ADD"))
	{
		if (!parv[1])
		{
			command_fail(si, fault_badparams, STR_INSUFFICIENT_PARAMS, "AJOIN");
			command_fail(si, fault_badparams, "Syntax: AJOIN <list|add|del|clear> [#channel]");
			return;
		}

		if ((md = metadata_find(si->smu, "private:autojoin")))
		{
			strlcpy(buf, md->value, sizeof buf);

			chan = strtok(buf, ",");
			while (chan != NULL)
			{
				if (!strcasecmp(chan, parv[1]))
				{
					command_fail(si, fault_badparams, "%s is already on your AJOIN list.", parv[1]);
					return;
				}
				chan = strtok(NULL, ",");
			}

			// Little arbitrary, but stop both overflow and RAM consumption going out of control
			if (strlen(md->value) + strlen(parv[1]) > 400)
			{
					command_fail(si, fault_badparams, "Sorry, you have too many AJOIN entries set.");
					return;
			}

			strlcpy(buf, md->value, sizeof buf);
			strncat(buf, ",", sizeof buf);
			strncat(buf, parv[1], sizeof buf);
			metadata_delete(si->smu, "private:autojoin");
			metadata_add(si->smu, "private:autojoin", buf);
		}
		else
		{
			metadata_add(si->smu, "private:autojoin", parv[1]);
		}
		command_success_nodata(si, "%s added to AJOIN successfully.", parv[1]);
	}
	else if (!strcasecmp(parv[0], "CLEAR"))
	{
		metadata_delete(si->smu, "private:autojoin");
		command_success_nodata(si, "AJOIN list cleared successfully.");
	}
	else if (!strcasecmp(parv[0], "DEL"))
	{
		if (!parv[1])
		{
			command_fail(si, fault_badparams, STR_INSUFFICIENT_PARAMS, "AJOIN");
			command_fail(si, fault_badparams, "Syntax: AJOIN <list|add|del|clear> [#channel]");
			return;
		}

		if (!(md = metadata_find(si->smu, "private:autojoin")))
		{
			command_fail(si, fault_badparams, "%s is not on your AJOIN list.", parv[1]);
			return;
		}

		// Thanks to John Brooks for his help with this.
		char *list = md->value;
		char *remove = parv[1];

		int listlen = 0;
		int rmlen = 0;
		int itempos = 0;
		int i = 0, j = 0;
		// This loop will find the item (if present), find the length of the item, and find the length of the entire string.
		for (; list[i]; i++)
		{
			if (!rmlen)
			{
				// We have not found the string yet
				if (tolower(list[i]) == tolower(remove[j]))
				{
					if (j == 0)
					{
						// First character of a potential match; remember it's location
						itempos = i;
					}
		 
					j++;
					if (!remove[j])
					{
						// Found the entire string
						rmlen = j;
					}
				}
				else
					j = 0;
			}
		}
		 
		if (remove[j])
		{
			// No match
			return;
		}
		 
		listlen = i;
		 
		// listlen is the length of the list, rmlen is the length of the item to remove, itempos is the beginning of that item.
		if (!list[itempos + rmlen])
		{
			// This item is the last item in the list, so we can simply truncate
			metadata_delete(si->smu, "private:autojoin");
		}
		else
		{
			// There are items after this one, so we must copy memory
			// Account for the comma following this item (if there is a space, account for that too, depends on how you format your list)
			rmlen += 1;
			memmove(list + itempos, list + itempos + rmlen, listlen - rmlen - itempos);
			list[listlen - rmlen] = '\0';
		}

		command_success_nodata(si, "%s removed from AJOIN successfully.", parv[1]);
	}
}

command_t ns_ajoin = { "AJOIN", "Manages automatic-join on identify.", AC_NONE, 2, ns_cmd_ajoin };

void _modinit(module_t *m)
{
	MODULE_USE_SYMBOL(ns_cmdtree, "nickserv/main", "ns_cmdtree");

	hook_add_event("user_identify");
	hook_add_hook("user_identify", ajoin_on_identify);
	command_add(&ns_ajoin, ns_cmdtree);
}

void _moddeinit(void)
{
	hook_del_hook("user_identify", ajoin_on_identify);
	command_delete(&ns_ajoin, ns_cmdtree);
}

static void ajoin_on_identify(void *vptr)
{
	user_t *u = vptr;
	myuser_t *mu = u->myuser;
	metadata_t *md;
	char buf[512];
	char *chan;

	if (!(md = metadata_find(mu, "private:autojoin")))
		return;

	strlcpy(buf, md->value, sizeof buf);
	chan = strtok(buf, " ,");
	while (chan != NULL)
	{
		if(ircd->type == PROTOCOL_SHADOWIRCD)
		{
			sts(":%s ENCAP * SVSJOIN %s %s", ME, CLIENT_NAME(u), chan);
		}
		else
		{
			sts(":%s SVSJOIN %s %s", CLIENT_NAME(nicksvs.me->me), CLIENT_NAME(u), chan);
		}

		chan = strtok(NULL, ",");
	}
}
