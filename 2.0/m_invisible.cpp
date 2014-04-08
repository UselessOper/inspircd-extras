/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2010 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include <stdarg.h>

/* $ModDesc: Allows for opered clients to join channels without being seen, similar to unreal 3.1 +I mode */
/* $ModDepends: core 2.0 */

class InvisibleMode : public ModeHandler
{
        int& uuidnick;

 public:
        InvisibleMode(Module* Creator, int& uidnick) : ModeHandler(Creator, "invis-oper", 'Q', PARAM_NONE, MODETYPE_USER), uuidnick(uidnick)
        {
                oper = true;
        }

        ~InvisibleMode()
        {
        }

        ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string &parameter, bool adding)
        {
                if (dest->IsModeSet('Q') != adding)
                {
                        dest->SetMode('Q', adding);

                        /* Fix for bug #379 reported by stealth. On +/-Q make m_watch think the user has signed on/off */
                        Module* m = ServerInstance->Modules->Find("m_watch.so");

                        /* This must come before setting/unsetting the handler */
                        if (m && adding)
                                m->OnUserQuit(dest, "Connection closed", "Connection closed");

                        /* User appears to vanish or appear from nowhere */
                        for (UCListIter f = dest->chans.begin(); f != dest->chans.end(); f++)
                        {
                                const UserMembList *ulist = (*f)->GetUsers();
                                char tb[MAXBUF];

                                snprintf(tb,MAXBUF,":%s %s %s", dest->GetFullHost().c_str(), adding ? "PART" : "JOIN", (*f)->name.c_str());
                                std::string out = tb;
                                Membership* memb = (**f).GetUser(dest);
                                std::string ms = memb->modes;
                                for(unsigned int i=0; i < memb->modes.length(); i++)
                                        ms.append(" ").append(dest->nick);


                                for (UserMembCIter i = ulist->begin(); i != ulist->end(); i++)
                                {
                                        /* User only appears to vanish for non-opers */
                                        if (IS_LOCAL(i->first) && !IS_OPER(i->first))
                                        {
                                                i->first->Write(out);
                                                if (!ms.empty() && !adding)
                                                        i->first->WriteServ("MODE %s +%s", (**f).name.c_str(), ms.c_str());
                                        }
                                }
                        }

                        if (adding && uuidnick)
                                dest->ForceNickChange(dest->uuid.c_str());

                        ServerInstance->SNO->WriteToSnoMask('a', "\2NOTICE\2: Oper %s has become %svisible (%cQ)", dest->GetFullHost().c_str(), adding ? "in" : "", adding ? '+' : '-');
                        return MODEACTION_ALLOW;
                }
                else
                {
                        return MODEACTION_DENY;
                }
        }
};

class ModuleInvisible : public Module
{
 private:
        InvisibleMode qm;
        enum
        {
                HIDE_CHAN = 0,
                HIDE_FULL = 1
        } HideLevel;

        bool override_active;
        bool safety;

 public:
        ModuleInvisible() : qm(this, (int&)HideLevel), override_active(false)
        {
        }

        void init()
        {
                ServerInstance->Modules->AddService(qm);

                /* Yeah i know people can take this out. I'm not about to obfuscate code just to be a pain in the ass. */
                ServerInstance->Users->ServerNoticeAll("*** m_invisible.so has just been loaded on this network. For more information, please visit http://inspircd.org/wiki/Modules/invisible");
                Implementation eventlist[] = {
                        I_OnUserPreMessage, I_OnUserPreNotice, I_OnUserJoin,
                        I_OnBuildNeighborList, I_OnSendWhoLine, I_OnNamesListItem,
                        I_OnRehash, I_OnUserPreNotice, I_OnWhoisLine, I_OnRawMode,
                        I_OnPreMode, I_OnUserPreNick, I_OnUserPart, I_OnStats, I_OnUserPreKick
                };
                ServerInstance->Modules->Attach(eventlist, this, sizeof(eventlist)/sizeof(Implementation));
                OnRehash(NULL);
        }

        void OnRehash(User*)
        {
                ConfigTag* tag = ServerInstance->Config->ConfValue("invisible");
                safety = tag->getBool("safe", false);
                HideLevel = tag->getBool("full", false) ? HIDE_FULL : HIDE_CHAN;
        }

        Version GetVersion()
        {
                return Version("Allows opers to join channels invisibly", VF_COMMON);
        }

        static void BuildExcept(Membership* memb, CUList& excepts)
        {
                const UserMembList* users = memb->chan->GetUsers();
                for(UserMembCIter i = users->begin(); i != users->end(); i++)
                        if (memb->user != i->first && IS_LOCAL(i->first) && !i->first->HasPrivPermission("invisible/see"))
                                excepts.insert(i->first);
        }

        void OnUserJoin(Membership* memb, bool sync, bool created, CUList& excepts)
        {
                if (memb->user->IsModeSet('Q'))
                {
                        BuildExcept(memb, excepts);
                        ServerInstance->SNO->WriteToSnoMask('a', "\2NOTICE\2: Oper %s has joined %s invisibly (+Q)",
                                memb->user->GetFullHost().c_str(), memb->chan->name.c_str());
                }
        }

        void OnUserPart(Membership* memb, std::string& partmessage, CUList& except_list)
        {
                if (memb->user->IsModeSet('Q'))
                        BuildExcept(memb, except_list);
        }

        void OnBuildNeighborList(User* source, UserChanList &include, std::map<User*,bool> &exceptions)
        {
                if (HideLevel == HIDE_FULL && source->IsModeSet('Q'))
                        include.clear();
        }

        ModResult OnUserPreKick(User* source, Membership* memb, const std::string& reason)
        {
                if (!IS_LOCAL(source) || !memb->user->IsModeSet('Q'))
                        return MOD_RES_PASSTHRU;

                source->WriteNumeric(401, "%s %s :No such nick/channel", source->nick.c_str(), memb->user->nick.c_str());
                return MOD_RES_DENY;
        }

        ModResult OnStats(char symbol, User* user, std::vector<std::string>& results)
        {
                if (HideLevel != HIDE_FULL || symbol != 'P' || IS_OPER(user))
                        return MOD_RES_PASSTHRU;

                // Steal some logic from cmd_stats.cpp
                unsigned int idx = 0;
                std::string sn(ServerInstance->Config->ServerName);
                for (std::list<User*>::const_iterator i = ServerInstance->Users->all_opers.begin(); i != ServerInstance->Users->all_opers.end(); ++i)
                {
                        User* oper = *i;
                        if (!ServerInstance->ULine(oper->server) && !oper->IsModeSet('Q'))
                        {
                                results.push_back(sn+" 249 " + user->nick + " :" + oper->nick + " (" + oper->ident + "@" + oper->dhost + ") Idle: " +
                                                (IS_LOCAL(oper) ? ConvToStr(ServerInstance->Time() - oper->idle_lastmsg) + " secs" : "unavailable"));
                                idx++;
                        }
                }
                results.push_back(sn+" 249 "+user->nick+" :"+ConvToStr(idx)+" OPER(s)");

                return MOD_RES_DENY;
        }

        ModResult OnUserPreNick(User* user, const std::string& newnick)
        {
                if (IS_LOCAL(user) && HideLevel == HIDE_FULL && user->IsModeSet('Q') && newnick != user->uuid)
                        return MOD_RES_DENY;

                return MOD_RES_PASSTHRU;
        }

        ModResult OnRawMode(User* user, Channel* chan, const char mode, const std::string& param, bool adding, int pcnt)
        {
                if (override_active || !chan | !IS_LOCAL(user))
                        return MOD_RES_PASSTHRU;

                User* nick = ServerInstance->FindNick(param);
                ModeHandler* mh = ServerInstance->Modes->FindMode(mode, MODETYPE_CHANNEL);
                if (!nick || !mh->GetPrefixRank())
                        return MOD_RES_PASSTHRU;

                if (nick->IsModeSet('Q'))
                {
                        user->WriteNumeric(401, "%s %s :No such nick/channel",user->nick.c_str(), param.c_str());
                        return MOD_RES_DENY;
                }

                return MOD_RES_PASSTHRU;
        }

        ModResult OnPreMode(User* source, User* dest, Channel* channel, const std::vector<std::string>& parameters)
        {
                if (override_active || !IS_LOCAL(source))
                        return MOD_RES_PASSTHRU;

                if (safety && channel && source->IsModeSet('Q'))
                {
                        if (parameters.size() < 2 || parameters[1] != "!")
                        {
                                source->WriteServ("NOTICE %s :*** Blocked message. You are hidden and safety is on. Place ' ! ' between channel and modes to override.", source->nick.c_str());
                                return MOD_RES_DENY;
                        }

                        std::vector<std::string> params = parameters;
                        params.erase(params.begin() + 1);
                        override_active = true;
                        ServerInstance->Modes->Process(params, source);
                        override_active = false;
                        return MOD_RES_DENY;
                }
                return MOD_RES_PASSTHRU;
        }

        ModResult OnWhoisLine(User* user, User* dest, int& numeric, std::string& text)
        {
                if (IS_OPER(user) || !dest->IsModeSet('Q'))
                        return MOD_RES_PASSTHRU;

                if (numeric == 318)
                        return MOD_RES_PASSTHRU;

                if (HideLevel == HIDE_FULL || numeric == 319)
                        return MOD_RES_DENY;

                return MOD_RES_PASSTHRU;
        }

        /* No privmsg response when hiding - submitted by Eric at neowin */
        ModResult OnUserPreNotice(User* user,void* dest,int target_type, std::string &text, char status, CUList &exempt_list)
        {
                if (HideLevel == HIDE_FULL && IS_LOCAL(user) && user->IsModeSet('Q'))
                {
                        // Don't block messages to other opers
                        if (target_type == TYPE_USER && IS_OPER(static_cast<User*>(dest)))
                                return MOD_RES_PASSTHRU;

                        if (text[0] != '!')
                        {
                                user->WriteServ("NOTICE %s :*** Blocked message. You are hidden and safety is on. Prefix message with ! to override.", user->nick.c_str());
                                return MOD_RES_DENY;
                        }
                        text = text.substr(1);
                }
                else if ((target_type == TYPE_USER) && (IS_LOCAL(user)))
                {
                        User* target = (User*)dest;
                        if(HideLevel == HIDE_FULL && target->IsModeSet('Q') && !IS_OPER(user))
                        {
                                user->WriteNumeric(401, "%s %s :No such nick/channel",user->nick.c_str(), target->nick.c_str());
                                return MOD_RES_DENY;
                        }
                }
                return MOD_RES_PASSTHRU;
        }

        ModResult OnUserPreMessage(User* user,void* dest,int target_type, std::string &text, char status, CUList &exempt_list)
        {
                return OnUserPreNotice(user, dest, target_type, text, status, exempt_list);
        }

        void OnSendWhoLine(User* source, const std::vector<std::string>& params, User* user, std::string& line)
        {
                if (!user->IsModeSet('Q') || IS_OPER(source))
                        return;
                if (HideLevel != HIDE_FULL)
                {
                        // Filthy stealing from cmd_who - easier than reliably just swapping out the parts we dont' care about
                        line = "352 " + source->nick + " " + "*" + " " + user->ident + " " +
                                (source->HasPrivPermission("users/auspex") ? user->host : user->dhost) + " ";
                        if (!ServerInstance->Config->HideWhoisServer.empty() && !source->HasPrivPermission("servers/auspex"))
                                line.append(ServerInstance->Config->HideWhoisServer);
                        else
                                line.append(user->server);

                        line.append(" " + user->nick + " ");

                        line += IS_AWAY(user) ? "G" : "H";

                        if (IS_OPER(user))
                                line.push_back('*');

                        line.append(" :0 " + user->fullname);
                }
                else
                        line.clear();

        }

        void OnNamesListItem(User* issuer, Membership* memb, std::string &prefixes, std::string &nick)
        {
                if (memb->user->IsModeSet('Q') && !IS_OPER(issuer))
                        nick.clear();
        }
};

MODULE_INIT(ModuleInvisible)
