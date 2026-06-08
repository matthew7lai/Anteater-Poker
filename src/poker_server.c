#define _DEFAULT_SOURCE  /* expose usleep() and related POSIX declarations */

#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "types.h"
#include "eval.h"

/* Configuration                                                             */

#define BOT_NAME_PREFIX   "Bot"
#define SMALL_BLIND       25
#define BIG_BLIND         50
#define MIN_RAISE_STEP    10
#define BOT_THINK_USEC    600000   /* artificial bot "thinking" delay        */
#define RESTART_DELAY_SEC 5        /* pause between hands                     */
#define INITIAL_BOTS      2        /* bots auto-added on startup             */
#define LISTEN_BACKLOG    8

/* Bot decision thresholds (percent), documented at point of use. */
#define BOT_RAISE_WHEN_FREE_PCT 25
#define BOT_FOLD_PCT            15
#define BOT_CALL_CUTOFF_PCT     75

/* Connection slots                                                          */


typedef struct {
    int       fd;       /**< Socket fd, or -1 for bots/empty.               */
    int       seat;     /**< Seat index this slot represents.               */
    int       is_bot;   /**< Non-zero if a bot occupies this seat.          */
    pthread_t thread;   /**< Per-client listener thread (humans only).      */
} ClientSlot;

/* Global server state                                                       */

static ClientSlot      g_slots[MAX_PLAYERS];
static Table           g_table;
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_server_fd  = -1;

/* Shuffled draw pile. The 52 ordinary cards occupy indices 0..51; the wild
 * card permanently occupies index 52 and is distributed separately. */
static Card g_deck[DECK_SIZE];
static int  g_deck_top = 0;

/* GTK dashboard widgets (main-thread use only). */
static GtkWidget     *g_player_list = NULL;
static GtkWidget     *g_round_label = NULL;
static GtkWidget     *g_pot_label   = NULL;
static GtkTextBuffer *g_log_buffer  = NULL;

/* Forward declarations                                                      */

static void  bot_act(int seat);
static void  advance_turn(void);
static void  next_round(void);
static void  deal_round(void);
static void *client_thread(void *arg);

/* Deck management                                                           */

/** Rebuild an ordered deck (52 standard + wild at index 52); reset the top. */
static void build_deck(void) {
    int i = 0;
    for (int s = 0; s < NUM_SUITS; s++) {
        for (int r = 0; r < NUM_RANKS; r++) {
            g_deck[i].rank    = (int8_t)r;
            g_deck[i].suit    = (int8_t)s;
            g_deck[i].is_wild = 0;
            i++;
        }
    }
    g_deck[52].rank    = WILD_RANK;
    g_deck[52].suit    = WILD_SUIT;
    g_deck[52].is_wild = 1;
    g_deck_top = 0;
}


static void shuffle_deck(void) {
    for (int i = 51; i > 0; i--) {
        int j = rand() % (i + 1);
        Card tmp  = g_deck[i];
        g_deck[i] = g_deck[j];
        g_deck[j] = tmp;
    }
}

/** Deal the next ordinary card, or a blank card if the pile is exhausted. */
static Card deal_one(void) {
    if (g_deck_top >= 52) {
        Card blank = {0, 0, 0};
        return blank;
    }
    return g_deck[g_deck_top++];
}

/* Logging (thread-safe via the GTK idle queue)                              */

/** GTK-thread callback that appends one line to the dashboard log. */
static gboolean append_log_idle(gpointer data) {
    char *msg = (char *)data;
    if (g_log_buffer != NULL) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(g_log_buffer, &end);
        gtk_text_buffer_insert(g_log_buffer, &end, msg, -1);
        gtk_text_buffer_insert(g_log_buffer, &end, "\n", 1);
    }
    free(msg);
    return G_SOURCE_REMOVE;
}

/** Log to stdout and (if the GUI exists) to the dashboard log, printf-style. */
static void server_log(const char *fmt, ...) {
    char *msg = malloc(MAX_MSG_LEN);
    if (msg == NULL) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, MAX_MSG_LEN, fmt, ap);
    va_end(ap);

    printf("[SERVER] %s\n", msg);
    fflush(stdout);

    if (g_log_buffer != NULL) {
        g_idle_add(append_log_idle, msg); /* ownership transferred */
    } else {
        free(msg);
    }
}

/* Message sending                                                           */

/** Send one newline-terminated, printf-formatted message to a single client. */
static void send_msg(int fd, const char *fmt, ...) {
    if (fd < 0) return;

    char buf[MAX_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap); /* reserve 1 for '\n' */
    va_end(ap);

    if (len < 0) return;
    if (len > (int)sizeof(buf) - 2) {
        len = (int)sizeof(buf) - 2; /* truncated but still well-formed */
    }
    buf[len]     = '\n';
    buf[len + 1] = '\0';

    send(fd, buf, strlen(buf), 0);
}

/** Broadcast a message to every connected human (bots have no socket). */
static void broadcast(const char *fmt, ...) {
    char buf[MAX_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_slots[i].is_bot && g_slots[i].fd >= 0) {
            send_msg(g_slots[i].fd, "%s", buf);
        }
    }
}

/** Broadcast the SEATS roster ("-" marks an empty seat). */
static void broadcast_seats(void) {
    char buf[MAX_MSG_LEN];
    int  used = snprintf(buf, sizeof(buf), "%s", MSG_SEATS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const char *name = g_table.players[i].active ? g_table.players[i].name : "-";
        used += snprintf(buf + used, sizeof(buf) - (size_t)used, "|%s", name);
    }
    broadcast("%s", buf);
}

/** Broadcast every seat's current point total. */
static void broadcast_points(void) {
    char buf[MAX_MSG_LEN];
    int  used = snprintf(buf, sizeof(buf), "%s", MSG_POINTS);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        used += snprintf(buf + used, sizeof(buf) - (size_t)used,
                         "|%d", g_table.players[i].points);
    }
    broadcast("%s", buf);
}

/* Dashboard rendering (GTK main thread)                                     */

/** Rebuild the dashboard's player table, round label, and pot label. */
static gboolean refresh_dashboard(gpointer data) {
    (void)data;
    if (g_player_list == NULL || g_round_label == NULL || g_pot_label == NULL) {
        return G_SOURCE_REMOVE;
    }

    GtkListStore *store =
        GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(g_player_list)));
    gtk_list_store_clear(store);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &g_table.players[i];
        if (!p->active) continue;

        const char *status = "Active";
        if (p->folded) status = "Folded";
        if (i == g_table.current_turn && !g_table.hand_over) status = ">> Turn <<";

        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
            0, p->name, 1, p->points, 2, p->current_bet, 3, status, -1);
    }

    static const char *const round_names[] = {
        "Pre-Flop", "Flop", "Turn", "River", "Showdown", "Waiting"
    };
    int r = (g_table.round < 0 || g_table.round > 4) ? 5 : g_table.round;
    gtk_label_set_text(GTK_LABEL(g_round_label), round_names[r]);

    char pot_str[32];
    snprintf(pot_str, sizeof(pot_str), "Pot: %d pts", g_table.pot);
    gtk_label_set_text(GTK_LABEL(g_pot_label), pot_str);

    return G_SOURCE_REMOVE;
}

/** Schedule a dashboard refresh on the GTK main thread. */
static void request_dashboard_refresh(void) {
    g_idle_add(refresh_dashboard, NULL);
}



/** Commit chips from a player into the pot, updating bet bookkeeping. */
static void commit_chips(Player *p, int amount) {
    if (amount < 0) amount = 0;
    if (amount > p->points) amount = p->points;
    p->points      -= amount;
    p->current_bet += amount;
    g_table.pot    += amount;
}

/** Drive one bot decision (check/call/raise/fold) and advance the turn. */
static void bot_act(int seat) {
    Player *p = &g_table.players[seat];
    if (!p->active || p->folded || g_table.hand_over) return;

    int to_call = g_table.current_bet - p->current_bet;
    int roll    = rand() % 100;

    if (to_call == 0) {
        /* Nothing to call: usually check, occasionally raise. */
        if (roll < BOT_RAISE_WHEN_FREE_PCT && p->points >= BIG_BLIND) {
            int raise_amt = BIG_BLIND + (rand() % 3) * BIG_BLIND;
            commit_chips(p, raise_amt);
            g_table.current_bet = p->current_bet;
            broadcast("%s|%s raises to %d", MSG_INFO, p->name, p->current_bet);
        } else {
            broadcast("%s|%s checks", MSG_INFO, p->name);
        }
    } else {
        /* Facing a bet: fold / call / raise by random thresholds. */
        if (roll < BOT_FOLD_PCT) {
            p->folded = 1;
            broadcast("%s|%s folds", MSG_INFO, p->name);
        } else if (roll < BOT_CALL_CUTOFF_PCT && to_call <= p->points) {
            commit_chips(p, to_call);
            broadcast("%s|%s calls %d", MSG_INFO, p->name, to_call);
        } else if (p->points >= to_call + BIG_BLIND) {
            int raise_amt = to_call + BIG_BLIND * (1 + rand() % 3);
            commit_chips(p, raise_amt);
            g_table.current_bet = p->current_bet;
            broadcast("%s|%s raises to %d", MSG_INFO, p->name, p->current_bet);
        } else if (to_call <= p->points) {
            commit_chips(p, to_call);
            broadcast("%s|%s calls %d", MSG_INFO, p->name, to_call);
        } else {
            p->folded = 1;
            broadcast("%s|%s folds", MSG_INFO, p->name);
        }
    }

    broadcast_points();
    advance_turn();
}

/* Turn / round progression (caller holds g_table_lock)                      */

/** Count players still contesting the pot (active and not folded). */
static int count_contenders(void) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_table.players[i].active && !g_table.players[i].folded) n++;
    }
    return n;
}

/** First active, non-folded seat clockwise from the dealer. */
static int first_to_act_seat(void) {
    int s = (g_table.dealer_seat + 1) % MAX_PLAYERS;
    while (!g_table.players[s].active || g_table.players[s].folded) {
        s = (s + 1) % MAX_PLAYERS;
    }
    return s;
}

/** Advance to the next actor, closing the round when betting is complete. */
static void advance_turn(void) {
    if (g_table.hand_over) return;

    if (count_contenders() <= 1) {
        next_round();
        return;
    }

    /* If everyone still in is all-in, run the board out automatically. */
    int all_allin = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &g_table.players[i];
        if (!p->active || p->folded) continue;
        if (p->points > 0) { all_allin = 0; break; }
    }
    if (all_allin) {
        next_round();
        return;
    }

    int all_matched = 1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &g_table.players[i];
        if (!p->active || p->folded) continue;
        if (p->current_bet < g_table.current_bet) { all_matched = 0; break; }
    }

    int start = g_table.current_turn;
    do {
        g_table.current_turn = (g_table.current_turn + 1) % MAX_PLAYERS;
    } while ((!g_table.players[g_table.current_turn].active ||
               g_table.players[g_table.current_turn].folded) &&
              g_table.current_turn != start);

    if (all_matched && g_table.current_turn == first_to_act_seat()) {
        next_round();
        return;
    }

    broadcast("%s|%d|%d|%d", MSG_TURN,
              g_table.current_turn, g_table.current_bet, g_table.pot);
    request_dashboard_refresh();

    if (g_slots[g_table.current_turn].is_bot) {
        usleep(BOT_THINK_USEC);
        bot_act(g_table.current_turn);
    }
}

/** Background timer: refill broke players and auto-start the next hand. */
static void *auto_restart_thread(void *arg) {
    (void)arg;
    sleep(RESTART_DELAY_SEC);

    pthread_mutex_lock(&g_table_lock);
    if (g_table.game_started) {
        pthread_mutex_unlock(&g_table_lock);
        return NULL;
    }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &g_table.players[i];
        if (!p->active || p->points > 0) continue;
        p->points = DEFAULT_POINTS;
        if (!g_slots[i].is_bot) {
            broadcast("%s|%s has been refilled with %d points!",
                      MSG_INFO, p->name, DEFAULT_POINTS);
        }
    }

    int ready = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_table.players[i].active && g_table.players[i].points > 0) ready++;
    }

    if (ready >= 2) {
        g_table.game_started = 1;
        pthread_mutex_unlock(&g_table_lock);
        deal_round();
    } else {
        g_table.game_started = 0;
        pthread_mutex_unlock(&g_table_lock);
        broadcast("%s|Not enough players to continue.", MSG_INFO);
    }
    return NULL;
}

/** Spawn the detached auto-restart timer. */
static void schedule_restart(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, auto_restart_thread, NULL) == 0) {
        pthread_detach(t);
    }
}

/** Reveal community cards for the current round and broadcast them. */
static void reveal_community(int count) {
    for (int i = 0; i < count; i++) {
        g_table.community[g_table.community_count++] = deal_one();
    }
    char buf[MAX_MSG_LEN];
    int  used = snprintf(buf, sizeof(buf), "%s", MSG_COMMUNITY);
    for (int i = 0; i < g_table.community_count; i++) {
        used += snprintf(buf + used, sizeof(buf) - (size_t)used, "|%d|%d",
                         g_table.community[i].rank, g_table.community[i].suit);
    }
    broadcast("%s", buf);
    server_log("Community: %d card(s) revealed", g_table.community_count);
}

/** Resolve the showdown: score hands, award the pot, broadcast results. */
static void resolve_showdown(void) {
    g_table.round     = ROUND_SHOWDOWN;
    g_table.hand_over = 1;

    int best_rank = -1;
    int best_tb[BEST_HAND_SIZE] = {-1, -1, -1, -1, -1};
    int winner = -1;
    int tie    = 0;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &g_table.players[i];
        if (!p->active || p->folded) continue;

        Card pool[MAX_PLAYERS + COMMUNITY_SIZE + HAND_SIZE];
        int  n = 0;
        pool[n++] = p->hand[0];
        pool[n++] = p->hand[1];
        if (p->wild_card.is_wild) pool[n++] = p->wild_card;
        for (int c = 0; c < g_table.community_count; c++) {
            pool[n++] = g_table.community[c];
        }

        Card best5[BEST_HAND_SIZE];
        char hand_name[32];
        int  rank = evaluate_best_hand(pool, n, best5, hand_name);

        for (int k = 0; k < BEST_HAND_SIZE; k++) {
            if (best5[k].is_wild) p->used_wild = 1;
        }

        int counts[NUM_RANKS] = {0};
        for (int k = 0; k < BEST_HAND_SIZE; k++) {
            if (best5[k].rank >= 0 && best5[k].rank < NUM_RANKS) {
                counts[best5[k].rank]++;
            }
        }
        int tb[BEST_HAND_SIZE];
        int oi = 0;
        for (int freq = 4; freq >= 1; freq--) {
            for (int r = NUM_RANKS - 1; r >= 0; r--) {
                if (counts[r] == freq) {
                    for (int k = 0; k < freq && oi < BEST_HAND_SIZE; k++) {
                        tb[oi++] = r;
                    }
                }
            }
        }
        while (oi < BEST_HAND_SIZE) tb[oi++] = -1;

        server_log("Showdown: %s has %s (rank %d)", p->name, hand_name, rank);

        int cmp = compare_hands(rank, tb, best_rank, best_tb);
        if (rank > best_rank || cmp > 0) {
            best_rank = rank;
            memcpy(best_tb, tb, sizeof(best_tb));
            winner = i;
            tie = 0;
            strncpy(g_table.winner_hand_name, hand_name,
                    sizeof(g_table.winner_hand_name) - 1);
            g_table.winner_hand_name[sizeof(g_table.winner_hand_name) - 1] = '\0';
        } else if (cmp == 0 && rank == best_rank) {
            tie = 1;
        }
    }

    int bonus = 0;
    if (!tie && winner >= 0) {
        g_table.winner_seat = winner;
        if (g_table.players[winner].used_wild) bonus = WILDCARD_BONUS;
        g_table.players[winner].points += g_table.pot + bonus;
        server_log("Winner: seat %d (%s) — %s  +%d pts%s",
                   winner, g_table.players[winner].name,
                   g_table.winner_hand_name, g_table.pot,
                   bonus ? " + WILD BONUS" : "");
    } else {
        g_table.winner_seat = WINNER_TIE;
        int share = g_table.pot / 2;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (g_table.players[i].active && !g_table.players[i].folded) {
                g_table.players[i].points += share;
            }
        }
    }

    broadcast("%s|%d|%s|%d|%d", MSG_RESULT,
              g_table.winner_seat, g_table.winner_hand_name, g_table.pot, bonus);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const Player *p = &g_table.players[i];
        if (!p->active) continue;
        broadcast("%s|%d|%s|%d|%d|%d|%d|%d", MSG_SHOWCARDS,
                  i, p->name,
                  p->hand[0].rank, p->hand[0].suit,
                  p->hand[1].rank, p->hand[1].suit,
                  p->wild_card.is_wild);
    }

    broadcast_points();
    g_table.pot          = 0;
    g_table.game_started = 0;
    request_dashboard_refresh();
    broadcast("%s|New hand starting in %d seconds...", MSG_INFO, RESTART_DELAY_SEC);
    schedule_restart();
}

/** Advance to the next betting round, or resolve the showdown. */
static void next_round(void) {
    /* Sole survivor wins immediately. */
    if (count_contenders() == 1) {
        int last = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (g_table.players[i].active && !g_table.players[i].folded) {
                last = i;
                break;
            }
        }
        g_table.hand_over   = 1;
        g_table.winner_seat = last;
        g_table.players[last].points += g_table.pot;
        snprintf(g_table.winner_hand_name, sizeof(g_table.winner_hand_name),
                 "Last Player Standing");
        broadcast("%s|%d|%s|%d|0", MSG_RESULT,
                  last, g_table.winner_hand_name, g_table.pot);
        broadcast_points();
        g_table.pot          = 0;
        g_table.game_started = 0;
        request_dashboard_refresh();
        broadcast("%s|New hand starting in %d seconds...", MSG_INFO, RESTART_DELAY_SEC);
        schedule_restart();
        return;
    }

    /* Reset per-round bets and advance the phase. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_table.players[i].current_bet = 0;
    }
    g_table.current_bet = 0;
    g_table.round++;

    if (g_table.round == ROUND_FLOP) {
        reveal_community(3);
    } else if (g_table.round == ROUND_TURN || g_table.round == ROUND_RIVER) {
        reveal_community(1);
    } else if (g_table.round >= ROUND_SHOWDOWN) {
        resolve_showdown();
        return;
    }

    /* New betting round starts left of the dealer. */
    g_table.current_turn = first_to_act_seat();
    broadcast("%s|%d|%d|%d", MSG_TURN,
              g_table.current_turn, g_table.current_bet, g_table.pot);
    request_dashboard_refresh();

    if (g_slots[g_table.current_turn].is_bot) {
        usleep(BOT_THINK_USEC);
        bot_act(g_table.current_turn);
    }
}

/* Dealing a new hand                                                        */

/** Next active seat at or after @p from (inclusive of wrapping). */
static int next_active_seat(int from) {
    int s = from % MAX_PLAYERS;
    while (!g_table.players[s].active) {
        s = (s + 1) % MAX_PLAYERS;
    }
    return s;
}

/** Set up and deal a fresh hand, post blinds, and prompt the first actor. */
static void deal_round(void) {
    pthread_mutex_lock(&g_table_lock);

    /* Refill broke bots; sit out broke humans. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &g_table.players[i];
        if (!p->active || p->points > 0) continue;
        if (g_slots[i].is_bot) {
            p->points = DEFAULT_POINTS;
        } else {
            broadcast("%s|%s is out of points and sits out!", MSG_INFO, p->name);
            p->active = 0;
        }
    }

    build_deck();
    shuffle_deck();

    g_table.pot             = 0;
    g_table.current_bet     = 0;
    g_table.community_count = 0;
    g_table.round           = ROUND_PREFLOP;
    g_table.hand_over       = 0;
    g_table.winner_seat     = WINNER_NONE;
    memset(g_table.winner_hand_name, 0, sizeof(g_table.winner_hand_name));

    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *p = &g_table.players[i];
        p->folded          = 0;
        p->current_bet     = 0;
        p->used_wild       = 0;
        p->wild_card.rank    = -1;
        p->wild_card.suit    = -1;
        p->wild_card.is_wild = 0;
    }

    /* Deal two hole cards to each active player. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_table.players[i].active) continue;
        g_table.players[i].hand[0] = deal_one();
        g_table.players[i].hand[1] = deal_one();
    }

    /* Give the wild card to one random active player. */
    int active_seats[MAX_PLAYERS];
    int active_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_table.players[i].active) active_seats[active_count++] = i;
    }
    if (active_count > 0) {
        int lucky = active_seats[rand() % active_count];
        g_table.players[lucky].wild_card = g_deck[52];
        server_log("Seat %d (%s) received the Anteater Wild Card",
                   lucky, g_table.players[lucky].name);
    }

    /* Rotate the dealer button to the next active seat. */
    g_table.dealer_seat = next_active_seat((g_table.dealer_seat + 1) % MAX_PLAYERS);

    int sb_seat = next_active_seat((g_table.dealer_seat + 1) % MAX_PLAYERS);
    int bb_seat = next_active_seat((sb_seat + 1) % MAX_PLAYERS);

    int sb_amt = (g_table.players[sb_seat].points >= SMALL_BLIND)
                 ? SMALL_BLIND : g_table.players[sb_seat].points;
    commit_chips(&g_table.players[sb_seat], sb_amt);

    int bb_amt = (g_table.players[bb_seat].points >= BIG_BLIND)
                 ? BIG_BLIND : g_table.players[bb_seat].points;
    commit_chips(&g_table.players[bb_seat], bb_amt);
    g_table.current_bet = bb_amt;

    server_log("Blinds: seat %d SB %d, seat %d BB %d",
               sb_seat, sb_amt, bb_seat, bb_amt);
    broadcast("%s|Blinds: %s posts %d, %s posts %d", MSG_INFO,
              g_table.players[sb_seat].name, sb_amt,
              g_table.players[bb_seat].name, bb_amt);

    g_table.current_turn = next_active_seat((bb_seat + 1) % MAX_PLAYERS);

    pthread_mutex_unlock(&g_table_lock);

    server_log("New hand dealt; dealer is seat %d", g_table.dealer_seat);
    request_dashboard_refresh();

    /* Send each player only their own cards. */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!g_table.players[i].active) continue;
        send_msg(g_slots[i].fd, "%s|%d|%d|%d|%d|%d|%d", MSG_DEAL,
                 g_table.players[i].hand[0].rank, g_table.players[i].hand[0].suit,
                 g_table.players[i].hand[1].rank, g_table.players[i].hand[1].suit,
                 g_table.players[i].wild_card.rank, g_table.players[i].wild_card.suit);
    }

    broadcast_points();
    broadcast("%s|%d|%d|%d", MSG_TURN,
              g_table.current_turn, g_table.current_bet, g_table.pot);

    if (g_slots[g_table.current_turn].is_bot) {
        usleep(BOT_THINK_USEC);
        pthread_mutex_lock(&g_table_lock);
        bot_act(g_table.current_turn);
        pthread_mutex_unlock(&g_table_lock);
    }
}

/* Human actions                                                             */

/** Apply a human player's action, validate it, and advance the game. */
static void process_action(int seat, const char *action, int amount) {
    pthread_mutex_lock(&g_table_lock);

    if (seat != g_table.current_turn) {
        send_msg(g_slots[seat].fd, "%s|Not your turn", MSG_ERROR);
        pthread_mutex_unlock(&g_table_lock);
        return;
    }

    Player *p = &g_table.players[seat];
    int to_call = g_table.current_bet - p->current_bet;

    if (strcmp(action, "FOLD") == 0) {
        p->folded = 1;
        broadcast("%s|%s folds", MSG_INFO, p->name);

    } else if (strcmp(action, "CALL") == 0) {
        int amt = (to_call > p->points) ? p->points : to_call;
        commit_chips(p, amt);
        broadcast("%s|%s calls %d", MSG_INFO, p->name, amt);

    } else if (strcmp(action, "RAISE") == 0) {
        if (amount <= to_call) amount = to_call + MIN_RAISE_STEP;
        commit_chips(p, amount);
        g_table.current_bet = p->current_bet;
        broadcast("%s|%s raises to %d", MSG_INFO, p->name, p->current_bet);

    } else if (strcmp(action, "CHECK") == 0) {
        if (to_call > 0) {
            send_msg(g_slots[seat].fd, "%s|Cannot check — must call %d",
                     MSG_ERROR, to_call);
            pthread_mutex_unlock(&g_table_lock);
            return;
        }
        broadcast("%s|%s checks", MSG_INFO, p->name);

    } else {
        send_msg(g_slots[seat].fd, "%s|Unknown action", MSG_ERROR);
        pthread_mutex_unlock(&g_table_lock);
        return;
    }

    broadcast_points();
    advance_turn();
    pthread_mutex_unlock(&g_table_lock);
}

/* Client and accept threads                                                 */

/** Per-client listener: parse and dispatch one player's protocol messages. */
static void *client_thread(void *arg) {
    int seat = *(int *)arg;
    free(arg);

    int  fd = g_slots[seat].fd;
    char buf[MAX_MSG_LEN];

    while (1) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';

        char *type = strtok(buf, "|");
        if (type == NULL) continue;

        if (strcmp(type, MSG_ACTION) == 0) {
            char *act = strtok(NULL, "|");
            char *amt = strtok(NULL, "|");
            if (act != NULL) {
                process_action(seat, act, amt ? atoi(amt) : 0);
            }

        } else if (strcmp(type, MSG_CHAT) == 0) {
            char *msg = strtok(NULL, "|");
            if (msg != NULL) {
                broadcast("%s|%s|%s", MSG_CHAT_BC, g_table.players[seat].name, msg);
            }

        } else if (strcmp(type, MSG_READY) == 0) {
            pthread_mutex_lock(&g_table_lock);
            int seated = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (g_table.players[i].active) seated++;
            }
            int should_start = (seated >= 2 && !g_table.game_started);
            if (should_start) g_table.game_started = 1;
            pthread_mutex_unlock(&g_table_lock);

            if (should_start) {
                broadcast("%s|Game starting!", MSG_INFO);
                sleep(1);
                deal_round();
            }

        } else if (strcmp(type, MSG_LEAVE) == 0) {
            break;
        }
    }

    pthread_mutex_lock(&g_table_lock);
    server_log("Player %s disconnected (seat %d)", g_table.players[seat].name, seat);
    g_table.players[seat].active = 0;
    g_slots[seat].fd = -1;
    pthread_mutex_unlock(&g_table_lock);

    broadcast_seats();
    close(fd);
    request_dashboard_refresh();
    return NULL;
}

/** Accept loop: validate JOIN, seat the player, and spawn a client thread. */
static void *accept_thread(void *arg) {
    (void)arg;
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);

    while (1) {
        int fd = accept(g_server_fd, (struct sockaddr *)&cli, &cli_len);
        if (fd < 0) break;

        char buf[MAX_MSG_LEN];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(fd); continue; }
        buf[n] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';

        char *type  = strtok(buf, "|");
        char *uname = strtok(NULL, "|");
        if (type == NULL || strcmp(type, MSG_JOIN) != 0 || uname == NULL) {
            close(fd);
            continue;
        }

        pthread_mutex_lock(&g_table_lock);
        int seat = -1;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!g_table.players[i].active) { seat = i; break; }
        }
        if (seat < 0) {
            send_msg(fd, "%s|Server full", MSG_ERROR);
            pthread_mutex_unlock(&g_table_lock);
            close(fd);
            continue;
        }

        Player *p = &g_table.players[seat];
        strncpy(p->name, uname, MAX_NAME_LEN - 1);
        p->name[MAX_NAME_LEN - 1] = '\0';
        p->seat   = seat;
        p->active = 1;
        p->points = DEFAULT_POINTS;
        p->folded = 0;

        g_slots[seat].fd     = fd;
        g_slots[seat].seat   = seat;
        g_slots[seat].is_bot = 0;
        pthread_mutex_unlock(&g_table_lock);

        send_msg(fd, "%s|%d|%d", MSG_WELCOME, seat, DEFAULT_POINTS);
        broadcast_seats();
        broadcast_points();
        server_log("Player '%s' joined seat %d from %s",
                   uname, seat, inet_ntoa(cli.sin_addr));
        request_dashboard_refresh();

        int *seat_arg = malloc(sizeof(int));
        if (seat_arg == NULL) { close(fd); g_slots[seat].fd = -1; continue; }
        *seat_arg = seat;
        if (pthread_create(&g_slots[seat].thread, NULL, client_thread, seat_arg) != 0) {
            free(seat_arg);
            close(fd);
            g_slots[seat].fd = -1;
        }
    }
    return NULL;
}

/* Bots                                                                      */

/** Fill up to @p count empty seats with bot players. */
static void add_bots(int count) {
    int added = 0;
    for (int i = 0; i < MAX_PLAYERS && added < count; i++) {
        if (g_table.players[i].active) continue;

        Player *p = &g_table.players[i];
        snprintf(p->name, MAX_NAME_LEN, "%s%d", BOT_NAME_PREFIX, added + 1);
        p->seat   = i;
        p->active = 1;
        p->points = DEFAULT_POINTS;
        p->folded = 0;

        g_slots[i].fd     = -1;
        g_slots[i].seat   = i;
        g_slots[i].is_bot = 1;
        added++;
        server_log("Bot '%s' added to seat %d", p->name, i);
    }
    request_dashboard_refresh();
}

/* GTK dashboard                                                             */

static void on_deal_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    int seated = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (g_table.players[i].active) seated++;
    }
    if (seated < 2) {
        server_log("Need at least 2 players to deal");
        return;
    }
    pthread_mutex_lock(&g_table_lock);
    g_table.game_started = 1;
    pthread_mutex_unlock(&g_table_lock);
    deal_round();
}

static void on_addbot_clicked(GtkButton *button, gpointer data) {
    (void)button; (void)data;
    pthread_mutex_lock(&g_table_lock);
    add_bots(1);
    pthread_mutex_unlock(&g_table_lock);
    broadcast_seats();
}

/** Build the host dashboard window and its widgets. */
static void build_server_gui(GtkApplication *app) {
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Anteater Poker — Server Dashboard");
    gtk_window_set_default_size(GTK_WINDOW(win), 700, 500);
    gtk_container_set_border_width(GTK_CONTAINER(win), 12);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "window{background:#0a2e1a;}"
        "label{color:#ccffcc;font-size:13px;}"
        "label.title{color:#ffe000;font-size:18px;font-weight:bold;}"
        "label.sub{color:#88ffaa;font-size:12px;}"
        "treeview{background:#0d3d22;color:#ccffcc;}"
        "textview text{background:#061a0e;color:#88ff88;"
        "font-family:monospace;font-size:11px;}", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    GtkWidget *title = gtk_label_new("Anteater Poker — Server Dashboard");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "title");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 4);

    GtkWidget *info_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(vbox), info_row, FALSE, FALSE, 0);
    g_round_label = gtk_label_new("Waiting");
    g_pot_label   = gtk_label_new("Pot: 0 pts");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_round_label), "sub");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_pot_label), "sub");
    gtk_box_pack_start(GTK_BOX(info_row), gtk_label_new("Round:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_row), g_round_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(info_row), g_pot_label, FALSE, FALSE, 16);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), btn_row, FALSE, FALSE, 0);
    GtkWidget *deal_btn = gtk_button_new_with_label("Deal New Hand");
    GtkWidget *bot_btn  = gtk_button_new_with_label("Add Bot");
    g_signal_connect(deal_btn, "clicked", G_CALLBACK(on_deal_clicked), NULL);
    g_signal_connect(bot_btn,  "clicked", G_CALLBACK(on_addbot_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btn_row), deal_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btn_row), bot_btn,  FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(4,
        G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING);
    g_player_list = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *const columns[] = { "Name", "Points", "Bet", "Status" };
    for (int i = 0; i < 4; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            columns[i], renderer, "text", i, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(g_player_list), col);
    }
    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(list_scroll), 150);
    gtk_container_add(GTK_CONTAINER(list_scroll), g_player_list);
    gtk_box_pack_start(GTK_BOX(vbox), list_scroll, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), gtk_label_new("Server Log"), FALSE, FALSE, 2);
    GtkWidget *log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    g_log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    GtkWidget *log_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(log_scroll), 180);
    gtk_container_add(GTK_CONTAINER(log_scroll), log_view);
    gtk_box_pack_start(GTK_BOX(vbox), log_scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(win);
}

/** GTK activation: build the GUI, start accepting, and seed bots. */
static void on_activate(GtkApplication *app, gpointer data) {
    (void)data;
    build_server_gui(app);
    server_log("Server started; listening on port %d", DEFAULT_PORT);

    pthread_t accept_tid;
    if (pthread_create(&accept_tid, NULL, accept_thread, NULL) == 0) {
        pthread_detach(accept_tid);
    }

    pthread_mutex_lock(&g_table_lock);
    add_bots(INITIAL_BOTS);
    pthread_mutex_unlock(&g_table_lock);
}

/* Entry point                                                               */

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));

    memset(&g_table, 0, sizeof(g_table));
    memset(g_slots,  0, sizeof(g_slots));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_slots[i].fd        = -1;
        g_table.players[i].seat = i;
    }
    g_table.dealer_seat  = -1;
    g_table.current_turn = 0;
    g_table.round        = -1;

    int port = DEFAULT_PORT;
    if (argc > 1) {
        int parsed = atoi(argv[1]);
        if (parsed > 0 && parsed < 65536) port = parsed;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(g_server_fd);
        return 1;
    }
    if (listen(g_server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(g_server_fd);
        return 1;
    }

    GtkApplication *app = gtk_application_new(
        "edu.uci.eecs22l.poker_server", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    close(g_server_fd);
    return status;
}
