/*
 * poker_client.c  —  Anteater Poker Client  (Team 23, EECS 22L)
 *
 * GTK3 graphical client.  Connects to poker_server over TCP/IP.
 * Shows hole cards, community cards, betting controls, chat, scoreboard.
 *
 * Build:  see src/Makefile
 * Run:    ./bin/poker_client [server_ip [port]]
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "types.h"

/* ─────────────────────────────────────────────────────
   Global client state
   ───────────────────────────────────────────────────── */
static int    server_fd   = -1;
static int    my_seat     = -1;
static int    my_points   = DEFAULT_POINTS;
static char   my_name[MAX_NAME_LEN] = "";

/* Local copy of game state (updated from server messages) */
static Table  ltable;
static Card   my_hand[HAND_SIZE];
static Card   my_wild;   /* rank=-1 means no wild this hand */
static int    my_turn    = 0;
static int    current_bet= 0;
static int    pot_val    = 0;
static int    player_pts[MAX_PLAYERS];
static char   seat_names[MAX_PLAYERS][MAX_NAME_LEN];

/* ── GTK Widgets ──────────────────────────────────────*/
static GtkWidget *main_stack;           /* switches login↔game screens */

/* Login screen */
static GtkWidget *entry_name, *entry_host, *entry_port;
static GtkWidget *login_status;

/* Game screen */
static GtkWidget *community_box;
static GtkWidget *hand_box;
static GtkWidget *wild_label;
static GtkWidget *pot_label;
static GtkWidget *pts_label;
static GtkWidget *turn_label;
static GtkWidget *status_label;
static GtkWidget *player_grid;
static GtkWidget *chat_view;
static GtkWidget *chat_entry;
static GtkTextBuffer *chat_buf;
static GtkWidget *btn_call, *btn_raise, *btn_fold, *btn_check;
static GtkWidget *raise_spin;

/* ─────────────────────────────────────────────────────
   CSS
   ───────────────────────────────────────────────────── */
static void apply_css(void) {
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p,
      "window{background:#0e3d1f;}"
      ".screen-title{color:#ffe000;font-size:22px;font-weight:900;}"
      ".card-lbl{"
      "  background:#fffef5;border:2px solid #888;border-radius:8px;"
      "  padding:6px 10px;font-size:20px;font-weight:bold;color:#111;"
      "  min-width:46px;}"
      ".card-lbl.red{color:#cc0000;}"
      ".card-lbl.wild{background:#fff3b0;border-color:#e0a000;color:#7a4800;}"
      ".card-lbl.community{background:#d0f5d0;border-color:#2a7a3a;font-size:22px;}"
      ".card-lbl.empty{background:#1e5a2e;border:2px dashed #3a9a4a;color:#3a9a4a;}"
      ".info-lbl{color:#ccffcc;font-size:13px;font-weight:bold;}"
      ".my-turn{color:#ffe000;font-size:14px;font-weight:bold;}"
      ".player-row{color:#aaffcc;font-size:12px;}"
      ".player-active{color:#ffe000;font-weight:bold;}"
      "button{font-size:13px;font-weight:bold;padding:5px 14px;}"
      ".btn-call{background:#1a7a3a;color:white;}"
      ".btn-raise{background:#7a5a00;color:white;}"
      ".btn-fold{background:#7a1a1a;color:white;}"
      ".btn-check{background:#1a4a7a;color:white;}"
      "entry{font-size:13px;}"
      "textview text{background:#061a0e;color:#88ff88;"
      "  font-family:monospace;font-size:11px;}"
      "separator{background:#2a6a3a;margin:3px 0;}",
      -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static void add_cls(GtkWidget *w, const char *c) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), c);
}

/* ─────────────────────────────────────────────────────
   Card widget factory
   ───────────────────────────────────────────────────── */
static GtkWidget *make_card_widget(const Card *c, gboolean community) {
    char buf[16];
    GtkWidget *lbl;
    if (c->rank < 0) {
        lbl = gtk_label_new("?");
        add_cls(lbl, "card-lbl"); add_cls(lbl, "empty");
        return lbl;
    }
    if (c->is_wild) {
        lbl = gtk_label_new("🐜 WILD");
        add_cls(lbl, "card-lbl"); add_cls(lbl, "wild");
        return lbl;
    }
    snprintf(buf, sizeof(buf), "%s%s", RANK_STR[c->rank], SUIT_STR[c->suit]);
    lbl = gtk_label_new(buf);
    add_cls(lbl, "card-lbl");
    if (community) add_cls(lbl, "community");
    if (c->suit == 1 || c->suit == 2) add_cls(lbl, "red"); /* diamonds/hearts */
    return lbl;
}

/* ─────────────────────────────────────────────────────
   Chat append (called from GTK thread only)
   ───────────────────────────────────────────────────── */
static void chat_append(const char *line) {
    if (!chat_buf) return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(chat_buf, &end);
    gtk_text_buffer_insert(chat_buf, &end, line, -1);
    gtk_text_buffer_insert(chat_buf, &end, "\n", 1);
}

/* ─────────────────────────────────────────────────────
   UI refresh (always called via g_idle_add)
   ───────────────────────────────────────────────────── */
static void clear_box(GtkContainer *b) {
    GList *ch = gtk_container_get_children(b);
    for (GList *l=ch;l;l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(ch);
}

typedef struct { int type; } RefreshCmd;
#define RT_COMMUNITY 1
#define RT_HAND      2
#define RT_STATUS    3
#define RT_PLAYERS   4
#define RT_ALL       5

static gboolean do_refresh(gpointer data) {
    int type = GPOINTER_TO_INT(data);

    if (type == RT_COMMUNITY || type == RT_ALL) {
        clear_box(GTK_CONTAINER(community_box));
        for (int i = 0; i < COMMUNITY_SIZE; i++) {
            Card c = (i < ltable.community_count) ?
                      ltable.community[i] : (Card){-1,-1,0};
            GtkWidget *w = make_card_widget(&c, TRUE);
            gtk_box_pack_start(GTK_BOX(community_box), w, FALSE, FALSE, 4);
        }
        gtk_widget_show_all(community_box);
    }

    if (type == RT_HAND || type == RT_ALL) {
        clear_box(GTK_CONTAINER(hand_box));
        for (int i = 0; i < HAND_SIZE; i++) {
            GtkWidget *w = make_card_widget(&my_hand[i], FALSE);
            gtk_box_pack_start(GTK_BOX(hand_box), w, FALSE, FALSE, 4);
        }
        gtk_widget_show_all(hand_box);

        if (my_wild.rank >= 0) {
            gtk_label_set_text(GTK_LABEL(wild_label), "You have the 🐜 Anteater Wild Card!");
        } else {
            gtk_label_set_text(GTK_LABEL(wild_label), "");
        }
    }

    if (type == RT_STATUS || type == RT_ALL) {
        char pbuf[64]; snprintf(pbuf, 64, "Your Points: %d", my_points);
        gtk_label_set_text(GTK_LABEL(pts_label), pbuf);
        char potbuf[32]; snprintf(potbuf, 32, "Pot: %d", pot_val);
        gtk_label_set_text(GTK_LABEL(pot_label), potbuf);

        if (my_turn) {
            gtk_label_set_text(GTK_LABEL(turn_label), "⭐ YOUR TURN");
            gtk_widget_set_sensitive(btn_call,  TRUE);
            gtk_widget_set_sensitive(btn_raise, TRUE);
            gtk_widget_set_sensitive(btn_fold,  TRUE);
            gtk_widget_set_sensitive(btn_check, TRUE);
        } else {
            gtk_label_set_text(GTK_LABEL(turn_label), "Waiting...");
            gtk_widget_set_sensitive(btn_call,  FALSE);
            gtk_widget_set_sensitive(btn_raise, FALSE);
            gtk_widget_set_sensitive(btn_fold,  FALSE);
            gtk_widget_set_sensitive(btn_check, FALSE);
        }
    }

    if (type == RT_PLAYERS || type == RT_ALL) {
        clear_box(GTK_CONTAINER(player_grid));
        int row = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (seat_names[i][0] == '\0' || strcmp(seat_names[i], "-")==0) continue;
            GtkWidget *name = gtk_label_new(seat_names[i]);
            GtkWidget *pts  = gtk_label_new("");
            char pbuf[32]; snprintf(pbuf, 32, "%d pts", player_pts[i]);
            gtk_label_set_text(GTK_LABEL(pts), pbuf);
            char seat_str[8]; snprintf(seat_str,8,"Seat %d",i);
            GtkWidget *seat_lbl = gtk_label_new(seat_str);
            add_cls(name, i==my_seat ? "player-active" : "player-row");
            add_cls(pts,  "player-row");
            add_cls(seat_lbl,"player-row");
            gtk_grid_attach(GTK_GRID(player_grid), seat_lbl, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(player_grid), name,     1, row, 1, 1);
            gtk_grid_attach(GTK_GRID(player_grid), pts,      2, row, 1, 1);
            row++;
        }
        gtk_widget_show_all(player_grid);
    }

    return G_SOURCE_REMOVE;
}

/* ─────────────────────────────────────────────────────
   Message parser (runs in recv thread, schedules GTK work)
   ───────────────────────────────────────────────────── */
static void handle_message(char *line) {
    char buf[MAX_MSG_LEN];
    strncpy(buf, line, MAX_MSG_LEN-1);

    char *tok = strtok(buf, "|");
    if (!tok) return;

    if (strcmp(tok, MSG_WELCOME)==0) {
        char *s = strtok(NULL,"|"); char *p = strtok(NULL,"|");
        if (s) my_seat = atoi(s);
        if (p) my_points = atoi(p);
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_STATUS));

    } else if (strcmp(tok, MSG_SEATS)==0) {
        for (int i=0;i<MAX_PLAYERS;i++) {
            char *n = strtok(NULL,"|");
            strncpy(seat_names[i], n ? n : "-", MAX_NAME_LEN-1);
        }
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_PLAYERS));

    } else if (strcmp(tok, MSG_DEAL)==0) {
        /* DEAL|r0|s0|r1|s1|wr|ws */
        int r0=atoi(strtok(NULL,"|")), s0=atoi(strtok(NULL,"|"));
        int r1=atoi(strtok(NULL,"|")), s1=atoi(strtok(NULL,"|"));
        int wr=atoi(strtok(NULL,"|")), ws=atoi(strtok(NULL,"|"));
        my_hand[0]=(Card){r0,s0,0}; my_hand[1]=(Card){r1,s1,0};
        if (wr>=0) my_wild=(Card){wr,ws,1};
        else       my_wild=(Card){-1,-1,0};
        ltable.community_count=0;
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_ALL));

    } else if (strcmp(tok, MSG_COMMUNITY)==0) {
        ltable.community_count=0;
        char *t;
        while ((t=strtok(NULL,"|")) && ltable.community_count < COMMUNITY_SIZE) {
            int r=atoi(t); t=strtok(NULL,"|"); int s=t?atoi(t):0;
            ltable.community[ltable.community_count++]=(Card){r,s,0};
        }
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_COMMUNITY));

    } else if (strcmp(tok, MSG_TURN)==0) {
        char *ts=strtok(NULL,"|"), *bs=strtok(NULL,"|"), *ps=strtok(NULL,"|");
        int turn_seat = ts ? atoi(ts) : -1;
        current_bet   = bs ? atoi(bs) : 0;
        pot_val       = ps ? atoi(ps) : 0;
        my_turn       = (turn_seat == my_seat);
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_STATUS));

    } else if (strcmp(tok, MSG_POINTS)==0) {
        for (int i=0;i<MAX_PLAYERS;i++) {
            char *v=strtok(NULL,"|");
            player_pts[i] = v ? atoi(v) : 0;
        }
        my_points = player_pts[my_seat >= 0 ? my_seat : 0];
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_PLAYERS));
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_STATUS));

    } else if (strcmp(tok, MSG_POT)==0) {
        char *v=strtok(NULL,"|"); if(v) pot_val=atoi(v);
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_STATUS));

    } else if (strcmp(tok, MSG_RESULT)==0) {
        char *ws=strtok(NULL,"|"), *hn=strtok(NULL,"|");
        char *pp=strtok(NULL,"|"), *bon=strtok(NULL,"|");
        int wseat = ws?atoi(ws):-1;
        int bonus  = bon?atoi(bon):0;
        char msg[256];
        if (wseat==-2)
            snprintf(msg,256,"🤝 It's a tie! Pot split.");
        else if (wseat==my_seat)
            snprintf(msg,256,"🎉 YOU WIN with %s! +%d pts%s",
                hn?hn:"?", pot_val, bonus?" + WILDCARD BONUS!":"");
        else
            snprintf(msg,256,"🏆 %s wins with %s%s",
                seat_names[wseat], hn?hn:"?", bonus?" (used Anteater Wild!)":"");
        gtk_label_set_text(GTK_LABEL(status_label), msg);
        /* append to chat too */
        char *msg_copy = g_strdup(msg);
        g_idle_add((GSourceFunc)chat_append, msg_copy);
        my_turn=0;
        g_idle_add(do_refresh, GINT_TO_POINTER(RT_STATUS));

    } else if (strcmp(tok, MSG_INFO)==0) {
        char *msg=strtok(NULL,"|");
        if (msg) {
            char *cp = g_strdup(msg);
            g_idle_add((GSourceFunc)chat_append, cp);
        }

    } else if (strcmp(tok, MSG_CHAT_BC)==0) {
        char *from=strtok(NULL,"|"); char *msg=strtok(NULL,"|");
        if (from&&msg) {
            char full[MAX_MSG_LEN];
            snprintf(full,MAX_MSG_LEN,"[%s] %s",from,msg);
            char *cp=g_strdup(full);
            g_idle_add((GSourceFunc)chat_append, cp);
        }

    } else if (strcmp(tok, MSG_ERROR)==0) {
        char *msg=strtok(NULL,"|");
        if (msg) {
            char full[MAX_MSG_LEN]; snprintf(full,MAX_MSG_LEN,"⚠ %s",msg);
            gtk_label_set_text(GTK_LABEL(status_label), full);
        }
    }
}

/* ─────────────────────────────────────────────────────
   Recv thread
   ───────────────────────────────────────────────────── */
static void *recv_thread(void *arg) {
    (void)arg;
    char buf[MAX_MSG_LEN*4];
    char line[MAX_MSG_LEN];
    int  lp = 0;

    while (1) {
        ssize_t n = recv(server_fd, buf, sizeof(buf)-1, 0);
        if (n <= 0) {
            g_idle_add((GSourceFunc)chat_append,
                g_strdup("⚠ Disconnected from server."));
            break;
        }
        buf[n] = 0;
        for (int i=0;i<n;i++) {
            if (buf[i]=='\n') {
                line[lp]=0; lp=0;
                if (strlen(line)>0) handle_message(line);
            } else if (lp < MAX_MSG_LEN-1) {
                line[lp++]=buf[i];
            }
        }
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────
   Send helper
   ───────────────────────────────────────────────────── */
static void send_to_server(const char *fmt, ...) {
    if (server_fd < 0) return;
    char buf[MAX_MSG_LEN];
    va_list ap; va_start(ap,fmt); vsnprintf(buf,MAX_MSG_LEN,fmt,ap); va_end(ap);
    strncat(buf,"\n",MAX_MSG_LEN-strlen(buf)-1);
    send(server_fd,buf,strlen(buf),0);
}

/* ─────────────────────────────────────────────────────
   Button callbacks
   ───────────────────────────────────────────────────── */
static void on_call(GtkButton *b,  gpointer d) { (void)b;(void)d;
    send_to_server("%s|CALL|0", MSG_ACTION); my_turn=0;
    g_idle_add(do_refresh,GINT_TO_POINTER(RT_STATUS)); }

static void on_fold(GtkButton *b,  gpointer d) { (void)b;(void)d;
    send_to_server("%s|FOLD|0", MSG_ACTION); my_turn=0;
    g_idle_add(do_refresh,GINT_TO_POINTER(RT_STATUS)); }

static void on_check(GtkButton *b, gpointer d) { (void)b;(void)d;
    send_to_server("%s|CHECK|0", MSG_ACTION); my_turn=0;
    g_idle_add(do_refresh,GINT_TO_POINTER(RT_STATUS)); }

static void on_raise(GtkButton *b, gpointer d) { (void)b;(void)d;
    int amt=(int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(raise_spin));
    send_to_server("%s|RAISE|%d", MSG_ACTION, amt); my_turn=0;
    g_idle_add(do_refresh,GINT_TO_POINTER(RT_STATUS)); }

static void on_chat_send(GtkButton *b, gpointer d) { (void)b;(void)d;
    const char *msg = gtk_entry_get_text(GTK_ENTRY(chat_entry));
    if (msg && strlen(msg)>0) {
        send_to_server("%s|%s", MSG_CHAT, msg);
        gtk_entry_set_text(GTK_ENTRY(chat_entry),"");
    }
}

/* ─────────────────────────────────────────────────────
   Login connect button
   ───────────────────────────────────────────────────── */
static void on_connect(GtkButton *b, gpointer d) {
    (void)b;(void)d;
    const char *name = gtk_entry_get_text(GTK_ENTRY(entry_name));
    const char *host = gtk_entry_get_text(GTK_ENTRY(entry_host));
    const char *port = gtk_entry_get_text(GTK_ENTRY(entry_port));

    if (!name||strlen(name)==0) {
        gtk_label_set_text(GTK_LABEL(login_status),"Please enter a username."); return; }

    strncpy(my_name, name, MAX_NAME_LEN-1);
    int portnum = port && strlen(port)>0 ? atoi(port) : DEFAULT_PORT;
    const char *hoststr = (host&&strlen(host)>0) ? host : "127.0.0.1";

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_port=htons(portnum);
    inet_pton(AF_INET, hoststr, &addr.sin_addr);

    if (connect(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
        gtk_label_set_text(GTK_LABEL(login_status),"Connection failed. Is server running?");
        close(server_fd); server_fd=-1; return;
    }

    /* Send JOIN */
    send_to_server("%s|%s", MSG_JOIN, my_name);

    /* Switch to game screen */
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack), "game");

    /* Start recv thread */
    pthread_t rt; pthread_create(&rt,NULL,recv_thread,NULL); pthread_detach(rt);

    /* Signal ready */
    sleep(0);
    send_to_server("%s", MSG_READY);
}

/* ─────────────────────────────────────────────────────
   Build Login Screen
   ───────────────────────────────────────────────────── */
static GtkWidget *build_login_screen(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL,12);
    gtk_container_set_border_width(GTK_CONTAINER(vbox),40);

    GtkWidget *title = gtk_label_new("🐜 Anteater Poker");
    add_cls(title,"screen-title");
    gtk_box_pack_start(GTK_BOX(vbox),title,FALSE,FALSE,8);

    GtkWidget *sub = gtk_label_new("UCI Edition — EECS 22L");
    add_cls(sub,"info-lbl");
    gtk_box_pack_start(GTK_BOX(vbox),sub,FALSE,FALSE,4);

    gtk_box_pack_start(GTK_BOX(vbox),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,8);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid),8);
    gtk_grid_set_column_spacing(GTK_GRID(grid),12);
    gtk_box_pack_start(GTK_BOX(vbox),grid,FALSE,FALSE,0);

    auto_add_field: ;
    GtkWidget *l1=gtk_label_new("Username:"); add_cls(l1,"info-lbl");
    entry_name=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name),"e.g. ZotFan42");
    gtk_grid_attach(GTK_GRID(grid),l1,0,0,1,1);
    gtk_grid_attach(GTK_GRID(grid),entry_name,1,0,1,1);

    GtkWidget *l2=gtk_label_new("Server IP:"); add_cls(l2,"info-lbl");
    entry_host=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_host),"127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid),l2,0,1,1,1);
    gtk_grid_attach(GTK_GRID(grid),entry_host,1,1,1,1);

    GtkWidget *l3=gtk_label_new("Port:"); add_cls(l3,"info-lbl");
    entry_port=gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry_port),"9000");
    gtk_grid_attach(GTK_GRID(grid),l3,0,2,1,1);
    gtk_grid_attach(GTK_GRID(grid),entry_port,1,2,1,1);

    GtkWidget *btn=gtk_button_new_with_label("Connect & Join Game");
    g_signal_connect(btn,"clicked",G_CALLBACK(on_connect),NULL);
    gtk_box_pack_start(GTK_BOX(vbox),btn,FALSE,FALSE,8);

    login_status=gtk_label_new("");
    add_cls(login_status,"info-lbl");
    gtk_box_pack_start(GTK_BOX(vbox),login_status,FALSE,FALSE,0);

    return vbox;
}

/* ─────────────────────────────────────────────────────
   Build Game Screen
   ───────────────────────────────────────────────────── */
static GtkWidget *build_game_screen(void) {
    GtkWidget *hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* ── Left panel: table ── */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_container_set_border_width(GTK_CONTAINER(left),10);
    gtk_paned_pack1(GTK_PANED(hpaned),left,TRUE,FALSE);

    GtkWidget *ttl=gtk_label_new("🐜 Anteater Poker");
    add_cls(ttl,"screen-title");
    gtk_box_pack_start(GTK_BOX(left),ttl,FALSE,FALSE,2);

    /* Status bar */
    GtkWidget *sbar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);
    gtk_box_pack_start(GTK_BOX(left),sbar,FALSE,FALSE,0);
    pot_label=gtk_label_new("Pot: 0"); add_cls(pot_label,"info-lbl");
    pts_label=gtk_label_new("Points: 1000"); add_cls(pts_label,"info-lbl");
    turn_label=gtk_label_new("Waiting..."); add_cls(turn_label,"my-turn");
    gtk_box_pack_start(GTK_BOX(sbar),pot_label,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(sbar),pts_label,FALSE,FALSE,0);
    gtk_box_pack_end  (GTK_BOX(sbar),turn_label,FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(left),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,2);

    /* Community cards */
    GtkWidget *comm_lbl=gtk_label_new("Community Cards"); add_cls(comm_lbl,"info-lbl");
    gtk_box_pack_start(GTK_BOX(left),comm_lbl,FALSE,FALSE,0);
    community_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(left),community_box,FALSE,FALSE,4);
    for (int i=0;i<COMMUNITY_SIZE;i++) {
        Card c={-1,-1,0};
        gtk_box_pack_start(GTK_BOX(community_box),make_card_widget(&c,TRUE),FALSE,FALSE,4);
    }

    gtk_box_pack_start(GTK_BOX(left),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,2);

    /* Your hand */
    GtkWidget *hand_lbl=gtk_label_new("Your Hand"); add_cls(hand_lbl,"info-lbl");
    gtk_box_pack_start(GTK_BOX(left),hand_lbl,FALSE,FALSE,0);
    hand_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(left),hand_box,FALSE,FALSE,4);
    for (int i=0;i<HAND_SIZE;i++) {
        Card c={-1,-1,0}; my_hand[i]=c;
        gtk_box_pack_start(GTK_BOX(hand_box),make_card_widget(&c,FALSE),FALSE,FALSE,4);
    }
    wild_label=gtk_label_new(""); add_cls(wild_label,"my-turn");
    gtk_box_pack_start(GTK_BOX(left),wild_label,FALSE,FALSE,2);

    gtk_box_pack_start(GTK_BOX(left),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,2);

    /* Action buttons */
    GtkWidget *act_box=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(left),act_box,FALSE,FALSE,4);

    btn_call =gtk_button_new_with_label("Call");  add_cls(btn_call, "btn-call");
    btn_fold =gtk_button_new_with_label("Fold");  add_cls(btn_fold, "btn-fold");
    btn_check=gtk_button_new_with_label("Check"); add_cls(btn_check,"btn-check");
    btn_raise=gtk_button_new_with_label("Raise"); add_cls(btn_raise,"btn-raise");
    raise_spin=gtk_spin_button_new_with_range(10,500,10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(raise_spin),50);

    g_signal_connect(btn_call, "clicked",G_CALLBACK(on_call), NULL);
    g_signal_connect(btn_fold, "clicked",G_CALLBACK(on_fold), NULL);
    g_signal_connect(btn_check,"clicked",G_CALLBACK(on_check),NULL);
    g_signal_connect(btn_raise,"clicked",G_CALLBACK(on_raise),NULL);

    gtk_widget_set_sensitive(btn_call, FALSE);
    gtk_widget_set_sensitive(btn_raise,FALSE);
    gtk_widget_set_sensitive(btn_fold, FALSE);
    gtk_widget_set_sensitive(btn_check,FALSE);

    gtk_box_pack_start(GTK_BOX(act_box),btn_call, FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(act_box),btn_check,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(act_box),btn_raise,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(act_box),raise_spin,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(act_box),btn_fold, FALSE,FALSE,0);

    status_label=gtk_label_new("Connecting to server...");
    add_cls(status_label,"info-lbl");
    gtk_box_pack_start(GTK_BOX(left),status_label,FALSE,FALSE,4);

    /* ── Right panel: players + chat ── */
    GtkWidget *right=gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_container_set_border_width(GTK_CONTAINER(right),10);
    gtk_paned_pack2(GTK_PANED(hpaned),right,FALSE,FALSE);

    GtkWidget *p_lbl=gtk_label_new("Players"); add_cls(p_lbl,"info-lbl");
    gtk_box_pack_start(GTK_BOX(right),p_lbl,FALSE,FALSE,0);
    player_grid=gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(player_grid),4);
    gtk_grid_set_column_spacing(GTK_GRID(player_grid),8);
    gtk_box_pack_start(GTK_BOX(right),player_grid,FALSE,FALSE,0);

    gtk_box_pack_start(GTK_BOX(right),gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),FALSE,FALSE,4);

    GtkWidget *c_lbl=gtk_label_new("Chat"); add_cls(c_lbl,"info-lbl");
    gtk_box_pack_start(GTK_BOX(right),c_lbl,FALSE,FALSE,0);
    GtkWidget *ctv=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctv),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctv),GTK_WRAP_WORD);
    chat_buf=gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctv));
    chat_view=ctv;
    GtkWidget *cscroll=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(cscroll),200);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(cscroll),220);
    gtk_container_add(GTK_CONTAINER(cscroll),ctv);
    gtk_box_pack_start(GTK_BOX(right),cscroll,TRUE,TRUE,0);

    GtkWidget *chat_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,4);
    gtk_box_pack_start(GTK_BOX(right),chat_row,FALSE,FALSE,0);
    chat_entry=gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(chat_entry),"Say something...");
    GtkWidget *send_btn=gtk_button_new_with_label("Send");
    g_signal_connect(send_btn,"clicked",G_CALLBACK(on_chat_send),NULL);
    g_signal_connect(chat_entry,"activate",G_CALLBACK(on_chat_send),NULL);
    gtk_box_pack_start(GTK_BOX(chat_row),chat_entry,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(chat_row),send_btn,FALSE,FALSE,0);

    return hpaned;
}

/* ─────────────────────────────────────────────────────
   App activate
   ───────────────────────────────────────────────────── */
static void on_activate(GtkApplication *app, gpointer data) {
    (void)data;
    apply_css();
    memset(&ltable,0,sizeof(ltable));
    for (int i=0;i<MAX_PLAYERS;i++) { player_pts[i]=0; seat_names[i][0]='\0'; }
    my_hand[0]=(Card){-1,-1,0}; my_hand[1]=(Card){-1,-1,0};
    my_wild=(Card){-1,-1,0};

    GtkWidget *win=gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win),"🐜 Anteater Poker — Client");
    gtk_window_set_default_size(GTK_WINDOW(win),920,600);

    main_stack=gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(main_stack),
        GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_container_add(GTK_CONTAINER(win),main_stack);

    gtk_stack_add_named(GTK_STACK(main_stack),build_login_screen(),"login");
    gtk_stack_add_named(GTK_STACK(main_stack),build_game_screen(), "game");
    gtk_stack_set_visible_child_name(GTK_STACK(main_stack),"login");

    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app=gtk_application_new("edu.uci.eecs22l.poker_client",
                                             G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(on_activate),NULL);
    int s=g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);
    if (server_fd>=0) { send_to_server("%s",MSG_LEAVE); close(server_fd); }
    return s;
}
