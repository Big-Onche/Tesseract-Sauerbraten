// creation of scoreboard
#include "game.h"

namespace game
{
    VARP(showservinfo, 0, 1, 1);
    VARP(showclientnum, 0, 0, 1);
    VARP(showpj, 0, 0, 1);
    VARP(showping, 0, 1, 2);
    VARP(showspectators, 0, 1, 1);
    VARP(showspectatorping, 0, 0, 1);
    VARP(highlightscore, 0, 1, 1);
    VARP(showconnecting, 0, 0, 1);
    VARP(hidefrags, 0, 1, 1);

    static hashset<teaminfo> teaminfos;

    void clearteaminfo()
    {
        teaminfos.clear();
    }

    void setteaminfo(const char *team, int frags)
    {
        teaminfo *t = teaminfos.access(team);
        if(!t) { t = &teaminfos[team]; copystring(t->team, team, sizeof(t->team)); }
        t->frags = frags;
    }

    static inline bool playersort(const fpsent *a, const fpsent *b)
    {
        if(a->state==CS_SPECTATOR)
        {
            if(b->state==CS_SPECTATOR) return strcmp(a->name, b->name) < 0;
            else return false;
        }
        else if(b->state==CS_SPECTATOR) return true;
        if(m_ctf || m_collect)
        {
            if(a->flags > b->flags) return true;
            if(a->flags < b->flags) return false;
        }
        if(a->frags > b->frags) return true;
        if(a->frags < b->frags) return false;
        return strcmp(a->name, b->name) < 0;
    }

    void getbestplayers(vector<fpsent *> &best)
    {
        loopv(players)
        {
            fpsent *o = players[i];
            if(o->state!=CS_SPECTATOR) best.add(o);
        }
        best.sort(playersort);
        while(best.length() > 1 && best.last()->frags < best[0]->frags) best.drop();
    }

    void getbestteams(vector<const char *> &best)
    {
        if(cmode && cmode->hidefrags())
        {
            vector<teamscore> teamscores;
            cmode->getteamscores(teamscores);
            teamscores.sort(teamscore::compare);
            while(teamscores.length() > 1 && teamscores.last().score < teamscores[0].score) teamscores.drop();
            loopv(teamscores) best.add(teamscores[i].team);
        }
        else
        {
            int bestfrags = INT_MIN;
            enumerate(teaminfos, teaminfo, t, bestfrags = max(bestfrags, t.frags));
            if(bestfrags <= 0) loopv(players)
            {
                fpsent *o = players[i];
                if(o->state!=CS_SPECTATOR && !teaminfos.access(o->team) && best.htfind(o->team) < 0) { bestfrags = 0; best.add(o->team); }
            }
            enumerate(teaminfos, teaminfo, t, if(t.frags >= bestfrags) best.add(t.team));
        }
    }

    struct scoregroup : teamscore
    {
        vector<fpsent *> players;
    };
    static vector<scoregroup *> groups;
    static vector<fpsent *> spectators;

    static inline bool scoregroupcmp(const scoregroup *x, const scoregroup *y)
    {
        if(!x->team)
        {
            if(y->team) return false;
        }
        else if(!y->team) return true;
        if(x->score > y->score) return true;
        if(x->score < y->score) return false;
        if(x->players.length() > y->players.length()) return true;
        if(x->players.length() < y->players.length()) return false;
        return x->team && y->team && strcmp(x->team, y->team) < 0;
    }

    static int groupplayers()
    {
        int numgroups = 0;
        spectators.setsize(0);
        loopv(players)
        {
            fpsent *o = players[i];
            if(!showconnecting && !o->name[0]) continue;
            if(o->state==CS_SPECTATOR) { spectators.add(o); continue; }
            const char *team = m_teammode && o->team[0] ? o->team : NULL;
            bool found = false;
            loopj(numgroups)
            {
                scoregroup &g = *groups[j];
                if(team!=g.team && (!team || !g.team || strcmp(team, g.team))) continue;
                g.players.add(o);
                found = true;
            }
            if(found) continue;
            if(numgroups>=groups.length()) groups.add(new scoregroup);
            scoregroup &g = *groups[numgroups++];
            g.team = team;
            if(!team) g.score = 0;
            else if(cmode && cmode->hidefrags()) g.score = cmode->getteamscore(o->team);
            else { teaminfo *ti = teaminfos.access(team); g.score = ti ? ti->frags : 0; }
            g.players.setsize(0);
            g.players.add(o);
        }
        loopi(numgroups) groups[i]->players.sort(playersort);
        spectators.sort(playersort);
        groups.sort(scoregroupcmp, 0, numgroups);
        return numgroups;
    }

    int statuscolor(fpsent *d, int color)
    {
        if(d->privilege)
        {
            color = d->privilege>=PRIV_ADMIN ? 0xFF8000 : (d->privilege>=PRIV_AUTH ? 0xC040C0 : 0x40FF80);
            if(d->state==CS_DEAD) color = (color>>1)&0x7F7F7F;
        }
        else if(d->state==CS_DEAD) color = 0x606060;
        return color;
    }

    VARP(hudscore, 0, 0, 1);
    FVARP(hudscorescale, 1e-3f, 1.0f, 1e3f);
    VARP(hudscorealign, -1, 0, 1);
    FVARP(hudscorex, 0, 0.50f, 1);
    FVARP(hudscorey, 0, 0.03f, 1);
    HVARP(hudscoreplayercolour, 0, 0x60A0FF, 0xFFFFFF);
    HVARP(hudscoreenemycolour, 0, 0xFF4040, 0xFFFFFF);
    VARP(hudscorealpha, 0, 255, 255);
    VARP(hudscoresep, 0, 200, 1000);

    void drawhudscore(int w, int h)
    {
        int numgroups = groupplayers();
        if(!numgroups) return;

        scoregroup *g = groups[0];
        int score = INT_MIN, score2 = INT_MIN;
        bool best = false;
        if(m_teammode)
        {
            score = g->score;
            best = isteam(player1->team, g->team);
            if(numgroups > 1)
            {
                if(best) score2 = groups[1]->score;
                else for(int i = 1; i < groups.length(); ++i) if(isteam(player1->team, groups[i]->team)) { score2 = groups[i]->score; break; }
                if(score2 == INT_MIN)
                {
                    fpsent *p = followingplayer(player1);
                    if(p->state==CS_SPECTATOR) score2 = groups[1]->score;
                }
            }
        }
        else
        {
            fpsent *p = followingplayer(player1);
            score = g->players[0]->frags;
            best = p == g->players[0];
            if(g->players.length() > 1)
            {
                if(best || p->state==CS_SPECTATOR) score2 = g->players[1]->frags;
                else score2 = p->frags;
            }
        }
        if(score == score2 && !best) best = true;

        score = clamp(score, -999, 9999);
        defformatstring(buf, "%d", score);
        int tw = 0, th = 0;
        text_bounds(buf, tw, th);

        string buf2;
        int tw2 = 0, th2 = 0;
        if(score2 > INT_MIN)
        {
            score2 = clamp(score2, -999, 9999);
            formatstring(buf2, "%d", score2);
            text_bounds(buf2, tw2, th2);
        }

        int fw = 0, fh = 0;
        text_bounds("00", fw, fh);
        fw = max(fw, max(tw, tw2));

        vec2 offset = vec2(hudscorex, hudscorey).mul(vec2(w, h).div(hudscorescale));
        if(hudscorealign == 1) offset.x -= 2*fw + hudscoresep;
        else if(hudscorealign == 0) offset.x -= (2*fw + hudscoresep) / 2.0f;
        vec2 offset2 = offset;
        offset.x += (fw-tw)/2.0f;
        offset.y -= th/2.0f;
        offset2.x += fw + hudscoresep + (fw-tw2)/2.0f;
        offset2.y -= th2/2.0f;

        pushhudscale(hudscorescale);

        int color = hudscoreplayercolour, color2 = hudscoreenemycolour;
        if(!best) swap(color, color2);

        draw_text(buf, int(offset.x), int(offset.y), (color>>16)&0xFF, (color>>8)&0xFF, color&0xFF, hudscorealpha);
        if(score2 > INT_MIN) draw_text(buf2, int(offset2.x), int(offset2.y), (color2>>16)&0xFF, (color2>>8)&0xFF, color2&0xFF, hudscorealpha);

        pophudmatrix();
    }

    void refreshscoreboard()
    {
        groupplayers();
    }

    COMMAND(refreshscoreboard, "");
    ICOMMAND(numscoreboard, "i", (int *team), intret(*team < 0 ? spectators.length() : (*team <= groups.length() ? groups[*team]->players.length() : 0)));
    ICOMMAND(loopscoreboard, "rie", (ident *id, int *team, uint *body),
    {

        if(*team > groupplayers()-1) return;
        vector<fpsent *> &p = *team < 0 ? spectators : groups[*team]->players;
        loopstart(id, stack);
        loopv(p)
        {
            loopiter(id, stack, p[i]->clientnum);
            execute(body);
        }
        loopend(id, stack);
    });

    ICOMMAND(scoreboardstatus, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d)
        {
            int status = d->state!=CS_DEAD ? 0xFFFFFF : 0x606060;
            if(d->privilege)
            {
                status = d->privilege>=PRIV_ADMIN ? 0xFF8000 : 0x40FF80;
                if(d->state==CS_DEAD) status = (status>>1)&0x7F7F7F;
            }
            intret(status);
        }
    });

    ICOMMAND(scoreboardping, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d)
        {
            if(!showpj && d->state==CS_LAGGED) result("LAG");
            else intret(d->ping);
        }
    });

    ICOMMAND(scoreboardshowfrags, "", (), intret(cmode && cmode->hidefrags() && hidefrags ? 0 : 1));
    ICOMMAND(scoreboardshowclientnum, "", (), intret(showclientnum || player1->privilege>=PRIV_MASTER ? 1 : 0));
    ICOMMAND(scoreboardmultiplayer, "", (), intret(multiplayer(false) || demoplayback ? 1 : 0));

    ICOMMAND(scoreboardhighlight, "i", (int *cn),
        intret(*cn == player1->clientnum && highlightscore && (multiplayer(false) || demoplayback || players.length() > 1) ? 0x808080 : 0));

    ICOMMAND(scoreboardservinfo, "", (),
    {
        if(!showservinfo) return;
        const ENetAddress *address = connectedpeer();
        if(address && player1->clientnum >= 0)
        {
            if(servinfo[0]) result(servinfo);
            else
            {
                string hostname;
                if(enet_address_get_host_ip(address, hostname, sizeof(hostname)) >= 0)
                    result(tempformatstring("%s:%d", hostname, address->port));
            }
        }
    });

    ICOMMAND(scoreboardmode, "", (),
    {
        result(server::modename(gamemode));
    });

    ICOMMAND(scoreboardmap, "", (),
    {
        const char *mname = getclientmap();
        result(mname[0] ? mname : "[new map]");
    });

    ICOMMAND(scoreboardtime, "", (),
    {
        if(m_timed && getclientmap() && (maplimit >= 0 || intermission))
        {
            if(intermission) result("intermission");
            else
            {
                int secs = max(maplimit-lastmillis + 999, 0)/1000;
                result(tempformatstring("%d:%02d", secs/60, secs%60));
            }
        }
    });

    const char *getclientname(int cn)
    {
        fpsent *d = getclient(cn);
        return d ? d->name : "";
    }
    ICOMMAND(getclientname, "i", (int *cn), result(getclientname(*cn)));

    ICOMMAND(getclientcolorname, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d) result(colorname(d));
    });

    int getclientteam(int cn)
    {
        fpsent *d = getclient(cn);
        return m_teammode && d ? !strcmp(d->team, "evil") ? 2 : 1 : 0;
    }
    ICOMMAND(getclientteam, "i", (int *cn), intret(getclientteam(*cn)));

    int getclientmodel(int cn)
    {
        fpsent *d = getclient(cn);
        return d ? d->playermodel : -1;
    }
    ICOMMAND(getclientmodel, "i", (int *cn), intret(getclientmodel(*cn)));

    //const char *getclienticon(int cn) TODO
    //{
    //    fpsent *d = getclient(cn);
    //    if(!d || d->state==CS_SPECTATOR) return "spectator";
    //    const playermodelinfo &mdl = getplayermodelinfo(d);
    //    return m_teammode ? mdl.icon[d->team] : mdl.icon[0];
    //}
    //ICOMMAND(getclienticon, "i", (int *cn), result(getclienticon(*cn)));

    ICOMMAND(getclientfrags, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d) intret(d->frags);
    });

    ICOMMAND(getclientflags, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d) intret(d->flags);
    });

    ICOMMAND(getclientdeaths, "i", (int *cn),
    {
        fpsent *d = getclient(*cn);
        if(d) intret(d->deaths);
    });

    ICOMMAND(getteamscore, "i", (int *team),
    {
        if(m_teammode && *team <= groupplayers())
        {
            string s;
            scoregroup &sg = *groups[*team-1];
            if(sg.score>=10000) formatstring(s, "%s: WIN", sg.team);
            else formatstring(s, "%s: %d", sg.team, sg.score);
            result(s);
        }
    });

    void showscores(bool on) { UI::holdui("scoreboard", on); }
}

