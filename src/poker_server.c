/*
 * poker_server.c  —  Anteater Poker Server  (Team 23, EECS 22L)
 *
 * Manages connections, game state, betting rounds, hand evaluation,
 * and bot players. Communicates with clients over TCP/IP sockets.
 *
 * Build:  see src/Makefile
 * Run:    ./bin/poker_server [port]   (default port 9000)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <gtk/gtk.h>

#include "types.h"
#include "eval.h"

/* ── Server state ───────────────────────────────────── */
#define BOT_PREFIX   "Bot"

typedef struct {
    int   fd;           /* socket fd, -1 if bot or empty */
    int   seat;
    int   is_bot;
    pthread_t thread;
} ClientSlot;

static ClientSlot   slots[MAX_PLAYERS];
static Table        table;
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static int          server_fd = -1;

/* GTK server dashboard widgets */
static GtkWidget   *log_textview   = NULL;
static GtkWidget   *player_list    = NULL;
static GtkWidget   *round_label    = NULL;
static GtkWidget   *pot_label      = NULL;
static GtkTextBuffer *log_buf      = NULL;

/* ── Deck helpers ───────────────────────────────────── */
static Card deck[DECK_SIZE];
static int  deck_top = 0;

static void build_deck(void) {
    int i = 0;
    for (int s = 0; s < 4; s++)
        for (int r = 0; r < 13; r++) {
            deck[i].rank = r; deck[i].suit = s; deck[i].is_wild = 0; i++;
        }
    /* Anteater Wild Card */
    deck[52].rank = 13; deck[52].suit = 4; deck[52].is_wild = 1;
    deck_top = 0;
}

static void shuffle_deck(void) {
    for (int i = DECK_SIZE-1; i > 0; i--) {
        int j = rand() % (i+1);
        Card t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
}

static Card deal_one(void) {
    if (deck_top >= DECK_SIZE) { Card c={0,0,0}; return c; }
    return deck[deck_top++];
}

/* ── Logging (thread-safe GTK idle) ─────────────────── */
typedef struct { char msg[512]; } LogMsg;

static gboolean _append_log(gpointer data) {
    LogMsg *m = (LogMsg*)data;
    if (log_buf) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(log_buf, &end);
        gtk_text_buffer_insert(log_buf, &end, m->msg, -1);
        gtk_text_buffer_insert(log_buf, &end, "\n", 1);
    }
    free(m);
    return G_SOURCE_REMOVE;
}

static void server_log(const char *fmt, ...) {
    LogMsg *m = malloc(sizeof(LogMsg));
    va_list ap; va_start(ap, fmt); vsnprintf(m->msg, 512, fmt, ap); va_end(ap);
    printf("[SERVER] %s\n", m->msg);
    fflush(stdout);
    if (log_buf) g_idle_add(_append_log, m);
    else free(m);
}

/* ── Send a message to one client ───────────────────── */
static void send_msg(int fd, const char *fmt, ...) {
    if (fd < 0) return;
    char buf[MAX_MSG_LEN];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, MAX_MSG_LEN, fmt, ap); va_end(ap);
    strncat(buf, "\n", MAX_MSG_LEN - strlen(buf) - 1);
    send(fd, buf, strlen(buf), 0);
}

/* Broadcast to all connected (non-bot) clients */
static void broadcast(const char *fmt, ...) {
    char buf[MAX_MSG_LEN];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, MAX_MSG_LEN, fmt, ap); va_end(ap);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (!slots[i].is_bot && slots[i].fd >= 0)
            send_msg(slots[i].fd, "%s", buf);
}

/* ── GTK dashboard refresh (idle callback) ──────────── */
static gboolean refresh_dashboard(gpointer data) {
    (void)data;
    if (!player_list || !round_label || !pot_label) return G_SOURCE_REMOVE;

    /* Update player list store */
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(player_list)));
    gtk_list_store_clear(store);
    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &table.players[i];
        if (!p->active) continue;
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        char status[32] = "Active";
        if (p->folded) snprintf(status, 32, "Folded");
        if (i == table.current_turn) snprintf(status, 32, ">> Turn <<");
        gtk_list_store_set(store, &iter,
            0, p->name,
            1, p->points,
            2, p->current_bet,
            3, status,
            -1);
    }
    static const char *round_names[] = {
        "Pre-Flop","Flop","Turn","River","Showdown","Waiting"
    };
    int r = table.round;
    if (r < 0 || r > 4) r = 5;
    gtk_label_set_text(GTK_LABEL(round_label), round_names[r]);
    char pot_str[32]; snprintf(pot_str, 32, "Pot: %d pts", table.pot);
    gtk_label_set_text(GTK_LABEL(pot_label), pot_str);
    pthread_mutex_unlock(&table_lock);
    return G_SOURCE_REMOVE;
}

/* ── Broadcast full seat list ───────────────────────── */
static void broadcast_seats(void) {
    char buf[MAX_MSG_LEN];
    snprintf(buf, MAX_MSG_LEN, "%s", MSG_SEATS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        strcat(buf, "|");
        if (table.players[i].active)
            strcat(buf, table.players[i].name);
        else
            strcat(buf, "-");
    }
    broadcast("%s", buf);
}

/* ── Broadcast points ───────────────────────────────── */
static void broadcast_points(void) {
    char buf[MAX_MSG_LEN];
    snprintf(buf, MAX_MSG_LEN, "%s", MSG_POINTS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        char tmp[16]; snprintf(tmp, 16, "|%d", table.players[i].points);
        strcat(buf, tmp);
    }
    broadcast("%s", buf);
}

/* ── Deal a round ────────────────────────────────────── */
static void deal_round(void) {
    pthread_mutex_lock(&table_lock);
    build_deck();
    shuffle_deck();

    table.pot = 0;
    table.current_bet = 0;
    table.community_count = 0;
    table.round = 0;
    table.hand_over = 0;
    table.winner_seat = -1;

    /* Deal 2 hole cards + check for wild card to each active player */
    /* Wild card is randomly distributed – only ONE player gets it   */
    int wild_dealt = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &table.players[i];
        if (!p->active) continue;
        p->folded = 0;
        p->current_bet = 0;
        p->used_wild = 0;
        p->hand[0] = deal_one();
        p->hand[1] = deal_one();
        /* 1-in-3 chance of getting wild (first eligible player gets it if needed) */
        if (!wild_dealt && (rand() % 3 == 0 || i == MAX_PLAYERS-1)) {
            p->wild_card = deck[52]; /* always the last card */
            wild_dealt = 1;
        } else {
            p->wild_card.rank = -1; p->wild_card.suit = -1; p->wild_card.is_wild = 0;
        }
    }
    /* If wild wasn't dealt yet assign to random active player */
    if (!wild_dealt) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (table.players[i].active) {
                table.players[i].wild_card = deck[52];
                break;
            }
        }
    }

    /* Advance dealer button */
    do {
        table.dealer_seat = (table.dealer_seat + 1) % MAX_PLAYERS;
    } while (!table.players[table.dealer_seat].active);

    /* First to act = seat after dealer */
    table.current_turn = (table.dealer_seat + 1) % MAX_PLAYERS;
    while (!table.players[table.current_turn].active)
        table.current_turn = (table.current_turn + 1) % MAX_PLAYERS;

    pthread_mutex_unlock(&table_lock);

    server_log("New hand dealt. Dealer: seat %d", table.dealer_seat);
    g_idle_add(refresh_dashboard, NULL);

    /* Send each player their cards */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &table.players[i];
        if (!p->active) continue;
        int fd = slots[i].fd;
        /* DEAL|r0|s0|r1|s1|wr|ws  (wr=-1 means no wild) */
        send_msg(fd, "%s|%d|%d|%d|%d|%d|%d",
            MSG_DEAL,
            p->hand[0].rank, p->hand[0].suit,
            p->hand[1].rank, p->hand[1].suit,
            p->wild_card.rank, p->wild_card.suit);
    }

    /* Announce whose turn it is */
    broadcast("%s|%d|%d|%d", MSG_TURN,
              table.current_turn, table.current_bet, table.pot);
}

/* ── Reveal community cards ─────────────────────────── */
static void reveal_community(int count) {
    /* reveal `count` new cards */
    for (int i = table.community_count; i < table.community_count + count; i++)
        table.community[i] = deal_one();

    char buf[MAX_MSG_LEN];
    snprintf(buf, MAX_MSG_LEN, "%s", MSG_COMMUNITY);
    for (int i = 0; i < table.community_count + count; i++) {
        char tmp[16];
        snprintf(tmp, 16, "|%d|%d", table.community[i].rank, table.community[i].suit);
        strcat(buf, tmp);
    }
    table.community_count += count;
    broadcast("%s", buf);
    server_log("Community: revealed %d total cards", table.community_count);
    g_idle_add(refresh_dashboard, NULL);
}

/* ── Advance betting round ───────────────────────────── */
static void next_round(void) {
    /* Reset bets */
    for (int i = 0; i < MAX_PLAYERS; i++) table.players[i].current_bet = 0;
    table.current_bet = 0;

    table.round++;
    if (table.round == 1) reveal_community(3);       /* flop  */
    else if (table.round == 2) reveal_community(1);  /* turn  */
    else if (table.round == 3) reveal_community(1);  /* river */
    else if (table.round >= 4) {
        /* Showdown */
        table.round = 4;
        table.hand_over = 1;

        /* Evaluate all active hands */
        int best_rank = -1;
        int best_tb[5] = {-1,-1,-1,-1,-1};
        int winner = -1;
        int tie = 0;

        for (int i = 0; i < MAX_PLAYERS; i++) {
            Player *p = &table.players[i];
            if (!p->active || p->folded) continue;

            /* Build pool: 2 hole + wild (if any) + community */
            Card pool[16]; int np = 0;
            pool[np++] = p->hand[0];
            pool[np++] = p->hand[1];
            if (p->wild_card.is_wild) pool[np++] = p->wild_card;
            for (int c = 0; c < table.community_count; c++)
                pool[np++] = table.community[c];

            Card best5[5]; char hname[32];
            int rank = evaluate_best_hand(pool, np, best5, hname);

            /* Check if wild was used in best5 */
            if (p->wild_card.is_wild)
                for (int k=0;k<5;k++)
                    if (best5[k].is_wild) p->used_wild=1;

            int tb[5]; eval_best_hand_tb: ;
            /* get tiebreak from eval */
            int dummy_tb[5];
            evaluate_best_hand(pool, np, best5, hname); /* already have rank */
            /* re-derive tb from rank+best5 */
            int cnt[13]={0};
            for(int k=0;k<5;k++) if(best5[k].rank>=0) cnt[best5[k].rank]++;
            int oi=0;
            for(int freq=4;freq>=1;freq--)
                for(int r=12;r>=0;r--)
                    if(cnt[r]==freq)
                        for(int k=0;k<freq&&oi<5;k++)
                            dummy_tb[oi++]=r;
            while(oi<5) dummy_tb[oi++]=-1;
            memcpy(tb, dummy_tb, sizeof(tb));

            server_log("Player %s: %s (rank %d)", p->name, hname, rank);

            int better = (rank > best_rank);
            if (!better && rank == best_rank) {
                int eq=1;
                for(int k=0;k<5;k++){
                    if(tb[k]>best_tb[k]){better=1;eq=0;break;}
                    if(tb[k]<best_tb[k]){eq=0;break;}
                }
                if(eq) tie=1;
            }
            if (better) {
                best_rank=rank; memcpy(best_tb,tb,sizeof(tb));
                winner=i; tie=0;
                strncpy(table.winner_hand_name, hname, 32);
            }
        }

        /* Award points */
        int bonus = 0;
        if (!tie && winner >= 0) {
            table.winner_seat = winner;
            if (table.players[winner].used_wild) bonus = WILDCARD_BONUS;
            table.players[winner].points += table.pot + bonus;
            server_log("Winner: seat %d (%s) — %s  +%d pts%s",
                winner, table.players[winner].name,
                table.winner_hand_name, table.pot,
                bonus ? " + WILDCARD BONUS!" : "");
        } else {
            table.winner_seat = -2; /* tie */
            /* split pot */
            int share = table.pot / 2; /* simplified */
            for (int i=0;i<MAX_PLAYERS;i++)
                if (table.players[i].active && !table.players[i].folded)
                    table.players[i].points += share;
        }

        broadcast("%s|%d|%s|%d|%d", MSG_RESULT,
            table.winner_seat, table.winner_hand_name, table.pot, bonus);
        broadcast_points();
        g_idle_add(refresh_dashboard, NULL);
        return;
    }

    /* Reset turn to player after dealer */
    table.current_turn = (table.dealer_seat + 1) % MAX_PLAYERS;
    while (!table.players[table.current_turn].active ||
            table.players[table.current_turn].folded)
        table.current_turn = (table.current_turn + 1) % MAX_PLAYERS;

    broadcast("%s|%d|%d|%d", MSG_TURN,
              table.current_turn, table.current_bet, table.pot);
    g_idle_add(refresh_dashboard, NULL);
}

/* ── Advance turn to next active player ─────────────── */
static void advance_turn(void) {
    int start = table.current_turn;
    do {
        table.current_turn = (table.current_turn + 1) % MAX_PLAYERS;
    } while ((!table.players[table.current_turn].active ||
               table.players[table.current_turn].folded)
             && table.current_turn != start);

    /* Count active players still in hand */
    int active_in_hand = 0;
    for (int i=0;i<MAX_PLAYERS;i++)
        if (table.players[i].active && !table.players[i].folded) active_in_hand++;

    if (active_in_hand <= 1) { next_round(); return; }

    /* Check if everyone has matched the current bet */
    int all_matched = 1;
    for (int i=0;i<MAX_PLAYERS;i++) {
        Player *p=&table.players[i];
        if (!p->active || p->folded) continue;
        if (p->current_bet < table.current_bet) { all_matched=0; break; }
    }
    if (all_matched && table.current_turn == (table.dealer_seat+1)%MAX_PLAYERS)
        { next_round(); return; }

    broadcast("%s|%d|%d|%d", MSG_TURN,
              table.current_turn, table.current_bet, table.pot);
    g_idle_add(refresh_dashboard, NULL);
}

/* ── Simple bot decision ─────────────────────────────── */
static void bot_act(int seat) {
    Player *p = &table.players[seat];
    /* Simple strategy: call if bet ≤ 100, fold otherwise (random raise) */
    int to_call = table.current_bet - p->current_bet;
    char action[16];
    int amount = 0;

    if (to_call == 0) {
        if (rand()%3 == 0) { snprintf(action,16,"RAISE"); amount=50; }
        else snprintf(action,16,"CHECK");
    } else if (to_call <= 100) {
        snprintf(action,16,"CALL");
    } else {
        snprintf(action,16,"FOLD");
    }

    server_log("Bot (seat %d) => %s %d", seat, action, amount);

    if (strcmp(action,"FOLD")==0) {
        p->folded=1;
        broadcast("%s|INFO|%s folds", MSG_INFO, p->name);
    } else if (strcmp(action,"CALL")==0) {
        p->points -= to_call; p->current_bet += to_call; table.pot += to_call;
        broadcast("%s|%s calls %d", MSG_INFO, p->name, to_call);
    } else if (strcmp(action,"RAISE")==0) {
        int raise = to_call + amount;
        p->points -= raise; p->current_bet += raise;
        table.current_bet = p->current_bet; table.pot += raise;
        broadcast("%s|%s raises to %d", MSG_INFO, p->name, p->current_bet);
    } else {
        broadcast("%s|%s checks", MSG_INFO, p->name);
    }
    advance_turn();
}

/* ── Process one action from a human client ─────────── */
static void process_action(int seat, char *action_str, int amount) {
    pthread_mutex_lock(&table_lock);
    Player *p = &table.players[seat];

    if (seat != table.current_turn) {
        send_msg(slots[seat].fd, "%s|Not your turn", MSG_ERROR);
        pthread_mutex_unlock(&table_lock);
        return;
    }

    int to_call = table.current_bet - p->current_bet;

    if (strcmp(action_str, "FOLD")==0) {
        p->folded=1;
        broadcast("%s|%s folds", MSG_INFO, p->name);
    } else if (strcmp(action_str, "CALL")==0) {
        if (to_call > p->points) to_call = p->points;
        p->points -= to_call; p->current_bet += to_call; table.pot += to_call;
        broadcast("%s|%s calls %d", MSG_INFO, p->name, to_call);
    } else if (strcmp(action_str, "RAISE")==0) {
        if (amount <= to_call) amount = to_call + 10;
        if (amount > p->points) amount = p->points;
        p->points -= amount; p->current_bet += amount;
        table.current_bet = p->current_bet; table.pot += amount;
        broadcast("%s|%s raises to %d", MSG_INFO, p->name, p->current_bet);
    } else if (strcmp(action_str, "CHECK")==0) {
        if (to_call > 0) {
            send_msg(slots[seat].fd, "%s|Cannot check — must call %d", MSG_ERROR, to_call);
            pthread_mutex_unlock(&table_lock);
            return;
        }
        broadcast("%s|%s checks", MSG_INFO, p->name);
    }

    broadcast_points();
    advance_turn();

    /* If next player is a bot, act immediately */
    while (table.players[table.current_turn].is_bot &&
           !table.hand_over &&
           table.players[table.current_turn].active &&
           !table.players[table.current_turn].folded) {
        bot_act(table.current_turn);
    }

    pthread_mutex_unlock(&table_lock);
}

/* ── Client handler thread ──────────────────────────── */
static void *client_thread(void *arg) {
    int seat = *(int*)arg; free(arg);
    int fd = slots[seat].fd;
    char buf[MAX_MSG_LEN];

    while (1) {
        ssize_t n = recv(fd, buf, MAX_MSG_LEN-1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        /* strip newline */
        buf[strcspn(buf, "\r\n")] = 0;

        char *tok = strtok(buf, "|");
        if (!tok) continue;

        if (strcmp(tok, MSG_ACTION)==0) {
            char *act = strtok(NULL, "|");
            char *amt = strtok(NULL, "|");
            int amount = amt ? atoi(amt) : 0;
            if (act) process_action(seat, act, amount);

        } else if (strcmp(tok, MSG_CHAT)==0) {
            char *msg = strtok(NULL, "|");
            if (msg) {
                broadcast("%s|%s|%s", MSG_CHAT_BC,
                          table.players[seat].name, msg);
                server_log("Chat [%s]: %s", table.players[seat].name, msg);
            }

        } else if (strcmp(tok, MSG_READY)==0) {
            /* check if enough players to start */
            pthread_mutex_lock(&table_lock);
            int cnt = 0;
            for (int i=0;i<MAX_PLAYERS;i++) if (table.players[i].active) cnt++;
            if (cnt >= 2 && !table.game_started) {
                table.game_started = 1;
                pthread_mutex_unlock(&table_lock);
                broadcast("%s|Game starting!", MSG_INFO);
                sleep(1);
                deal_round();
            } else {
                pthread_mutex_unlock(&table_lock);
            }

        } else if (strcmp(tok, MSG_LEAVE)==0) {
            break;
        }
    }

    /* Player left */
    pthread_mutex_lock(&table_lock);
    server_log("Player %s disconnected (seat %d)",
               table.players[seat].name, seat);
    table.players[seat].active = 0;
    slots[seat].fd = -1;
    pthread_mutex_unlock(&table_lock);

    broadcast_seats();
    close(fd);
    g_idle_add(refresh_dashboard, NULL);
    return NULL;
}

/* ── Accept thread ───────────────────────────────────── */
static void *accept_thread(void *arg) {
    (void)arg;
    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    while (1) {
        int fd = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (fd < 0) break;

        char name_buf[MAX_MSG_LEN];
        ssize_t n = recv(fd, name_buf, MAX_MSG_LEN-1, 0);
        if (n <= 0) { close(fd); continue; }
        name_buf[n] = 0; name_buf[strcspn(name_buf,"\r\n")]=0;

        /* Expect: JOIN|username */
        char *tok = strtok(name_buf,"|");
        char *uname = strtok(NULL,"|");
        if (!tok || strcmp(tok,MSG_JOIN)!=0 || !uname) { close(fd); continue; }

        /* Find empty seat */
        pthread_mutex_lock(&table_lock);
        int seat = -1;
        for (int i=0;i<MAX_PLAYERS;i++)
            if (!table.players[i].active) { seat=i; break; }

        if (seat < 0) {
            send_msg(fd, "%s|Server full", MSG_ERROR);
            pthread_mutex_unlock(&table_lock);
            close(fd); continue;
        }

        Player *p = &table.players[seat];
        strncpy(p->name, uname, MAX_NAME_LEN-1);
        p->seat = seat;
        p->active = 1;
        p->points = DEFAULT_POINTS;
        p->folded = 0;
        slots[seat].fd = fd;
        slots[seat].seat = seat;
        slots[seat].is_bot = 0;
        pthread_mutex_unlock(&table_lock);

        send_msg(fd, "%s|%d|%d", MSG_WELCOME, seat, DEFAULT_POINTS);
        broadcast_seats();
        broadcast_points();
        server_log("Player '%s' joined seat %d from %s",
                   uname, seat, inet_ntoa(cli_addr.sin_addr));
        g_idle_add(refresh_dashboard, NULL);

        /* Spawn handler thread */
        int *sarg = malloc(sizeof(int)); *sarg = seat;
        pthread_create(&slots[seat].thread, NULL, client_thread, sarg);
    }
    return NULL;
}

/* ── Add bots to fill empty seats ───────────────────── */
static void add_bots(int count) {
    int added = 0;
    for (int i = 0; i < MAX_PLAYERS && added < count; i++) {
        if (!table.players[i].active) {
            Player *p = &table.players[i];
            char bname[MAX_NAME_LEN];
            snprintf(bname, MAX_NAME_LEN, "%s%d", BOT_PREFIX, added+1);
            strncpy(p->name, bname, MAX_NAME_LEN-1);
            p->seat = i; p->active = 1; p->points = DEFAULT_POINTS;
            p->folded = 0; p->is_bot = 1;  /* flag on Player not in types.h, skip */
            slots[i].fd = -1; slots[i].seat = i; slots[i].is_bot = 1;
            added++;
            server_log("Bot '%s' added to seat %d", bname, i);
        }
    }
    g_idle_add(refresh_dashboard, NULL);
}

/* ── GTK server dashboard ────────────────────────────── */
static void on_deal_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    /* Count active players */
    int cnt=0;
    for(int i=0;i<MAX_PLAYERS;i++) if(table.players[i].active) cnt++;
    if(cnt < 2) { server_log("Need at least 2 players"); return; }
    table.game_started=1;
    deal_round();
}

static void on_addbot_clicked(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    pthread_mutex_lock(&table_lock);
    add_bots(1);
    pthread_mutex_unlock(&table_lock);
    broadcast_seats();
}

static void build_server_gui(GtkApplication *app) {
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Anteater Poker — Server Dashboard");
    gtk_window_set_default_size(GTK_WINDOW(win), 700, 500);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);

    /* CSS */
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window{background:#0a2e1a;}"
        "label{color:#ccffcc;font-size:13px;}"
        "label.title{color:#ffe000;font-size:18px;font-weight:bold;}"
        "label.sub{color:#88ffaa;font-size:12px;}"
        "button{font-weight:bold;}"
        "treeview{background:#0d3d22;color:#ccffcc;}"
        "textview text{background:#061a0e;color:#88ff88;font-family:monospace;font-size:11px;}",-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL,8);
    gtk_container_add(GTK_CONTAINER(win),vbox);

    GtkWidget *title = gtk_label_new("🐜 Anteater Poker — Server Dashboard");
    gtk_style_context_add_class(gtk_widget_get_style_context(title),"title");
    gtk_box_pack_start(GTK_BOX(vbox),title,FALSE,FALSE,4);

    /* Status row */
    GtkWidget *hrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,16);
    gtk_box_pack_start(GTK_BOX(vbox),hrow,FALSE,FALSE,0);
    round_label = gtk_label_new("Waiting");
    pot_label   = gtk_label_new("Pot: 0 pts");
    gtk_style_context_add_class(gtk_widget_get_style_context(round_label),"sub");
    gtk_style_context_add_class(gtk_widget_get_style_context(pot_label),"sub");
    gtk_box_pack_start(GTK_BOX(hrow),gtk_label_new("Round:"),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(hrow),round_label,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(hrow),pot_label,FALSE,FALSE,16);

    /* Buttons */
    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(vbox),btn_row,FALSE,FALSE,0);
    GtkWidget *deal_btn = gtk_button_new_with_label("Deal New Hand");
    GtkWidget *bot_btn  = gtk_button_new_with_label("Add Bot");
    g_signal_connect(deal_btn,"clicked",G_CALLBACK(on_deal_clicked),NULL);
    g_signal_connect(bot_btn, "clicked",G_CALLBACK(on_addbot_clicked),NULL);
    gtk_box_pack_start(GTK_BOX(btn_row),deal_btn,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(btn_row),bot_btn, FALSE,FALSE,0);

    /* Player list */
    GtkListStore *store = gtk_list_store_new(4,
        G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    player_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    const char *cols[] = {"Name","Points","Bet","Status"};
    for (int i=0;i<4;i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            cols[i],r,"text",i,NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(player_list),c);
    }
    GtkWidget *scroll1 = gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll1),150);
    gtk_container_add(GTK_CONTAINER(scroll1),player_list);
    gtk_box_pack_start(GTK_BOX(vbox),scroll1,FALSE,FALSE,0);

    /* Log */
    GtkWidget *log_lbl = gtk_label_new("Server Log");
    gtk_box_pack_start(GTK_BOX(vbox),log_lbl,FALSE,FALSE,2);
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv),FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv),FALSE);
    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    log_textview = tv;
    GtkWidget *scroll2 = gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll2),180);
    gtk_container_add(GTK_CONTAINER(scroll2),tv);
    gtk_box_pack_start(GTK_BOX(vbox),scroll2,TRUE,TRUE,0);

    gtk_widget_show_all(win);
}

static void on_activate(GtkApplication *app, gpointer data) {
    (void)data;
    build_server_gui(app);
    server_log("Server started. Listening on port %d", DEFAULT_PORT);

    /* Start accept thread */
    pthread_t at;
    pthread_create(&at, NULL, accept_thread, NULL);
    pthread_detach(at);

    /* Add 2 bots by default */
    pthread_mutex_lock(&table_lock);
    add_bots(2);
    pthread_mutex_unlock(&table_lock);
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));
    memset(&table,  0, sizeof(table));
    memset(slots, 0, sizeof(slots));
    for (int i=0;i<MAX_PLAYERS;i++) { slots[i].fd=-1; table.players[i].seat=i; }
    table.dealer_seat=-1; table.current_turn=0; table.round=-1;

    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    /* Create server socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt=1; setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(port);
    if (bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
        perror("bind"); exit(1);
    }
    listen(server_fd,8);

    GtkApplication *app = gtk_application_new("edu.uci.eecs22l.poker_server",
                                               G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app,"activate",G_CALLBACK(on_activate),NULL);
    int status = g_application_run(G_APPLICATION(app),argc,argv);
    g_object_unref(app);
    close(server_fd);
    return status;
}
