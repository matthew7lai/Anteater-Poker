#define _DEFAULT_SOURCE

#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "types.h"


/* UI refresh selectors                                                      */

typedef enum {
    REFRESH_COMMUNITY = 1,
    REFRESH_HAND      = 2,
    REFRESH_STATUS    = 3,
    REFRESH_PLAYERS   = 4,
    REFRESH_ALL       = 5
} RefreshKind;

/* Client state                                                              */

static int  g_server_fd = -1;
static int  g_my_seat   = -1;
static int  g_my_points = DEFAULT_POINTS;
static char g_my_name[MAX_NAME_LEN] = "";

static Table g_local_table;                       /* display mirror only      */
static Card  g_my_hand[HAND_SIZE];
static Card  g_my_wild;
static int   g_is_my_turn = 0;
static int   g_current_bet = 0;
static int   g_pot = 0;
static int   g_player_points[MAX_PLAYERS];
static char  g_seat_names[MAX_PLAYERS][MAX_NAME_LEN];

/* Widgets                                                                   */

static GtkWidget *g_main_stack;

/* Login screen */
static GtkWidget *g_entry_name;
static GtkWidget *g_entry_host;
static GtkWidget *g_entry_port;
static GtkWidget *g_login_status;

/* Game screen */
static GtkWidget     *g_community_box;
static GtkWidget     *g_hand_box;
static GtkWidget     *g_wild_label;
static GtkWidget     *g_pot_label;
static GtkWidget     *g_points_label;
static GtkWidget     *g_turn_label;
static GtkWidget     *g_status_label;
static GtkWidget     *g_player_grid;
static GtkWidget     *g_chat_entry;
static GtkTextBuffer *g_chat_buffer;
static GtkWidget     *g_btn_call, *g_btn_raise, *g_btn_fold, *g_btn_check, *g_btn_allin;
static GtkWidget     *g_raise_spin;

/* Forward declarations                                                      */

static void send_to_server(const char *fmt, ...);

/* Styling                                                                   */

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "window{background:#0e3d1f;}"
        ".screen-title{color:#ffe000;font-size:22px;font-weight:900;}"
        ".card-lbl{background:#fffef5;border:2px solid #888;border-radius:8px;"
        "  padding:6px 10px;font-size:20px;font-weight:bold;color:#111;min-width:46px;}"
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
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/** Shorthand to add a CSS class to a widget. */
static void add_class(GtkWidget *w, const char *cls) {
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* Card rendering                                                            */

/** Create a label widget representing one card (placeholder/wild/normal). */
static GtkWidget *make_card_widget(const Card *c, gboolean community) {
    if (c->rank < 0) {
        GtkWidget *lbl = gtk_label_new("?");
        add_class(lbl, "card-lbl");
        add_class(lbl, "empty");
        return lbl;
    }
    if (c->is_wild) {
        GtkWidget *lbl = gtk_label_new("WILD");
        add_class(lbl, "card-lbl");
        add_class(lbl, "wild");
        return lbl;
    }

    char text[16];
    snprintf(text, sizeof(text), "%s%s", RANK_STR[c->rank], SUIT_STR[c->suit]);
    GtkWidget *lbl = gtk_label_new(text);
    add_class(lbl, "card-lbl");
    if (community) add_class(lbl, "community");
    if (c->suit == 1 || c->suit == 2) add_class(lbl, "red"); /* diamonds/hearts */
    return lbl;
}

/* Chat helpers (GTK main thread only)                                       */

/** Append a line to the chat view; @p data is a heap string we take ownership of. */
static gboolean chat_append_idle(gpointer data) {
    char *line = (char *)data;
    if (g_chat_buffer != NULL) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(g_chat_buffer, &end);
        gtk_text_buffer_insert(g_chat_buffer, &end, line, -1);
        gtk_text_buffer_insert(g_chat_buffer, &end, "\n", 1);
    }
    g_free(line);
    return G_SOURCE_REMOVE;
}

/** Queue a chat line for display from any thread. */
static void chat_post(const char *line) {
    g_idle_add(chat_append_idle, g_strdup(line));
}

/** Remove every child widget from a container. */
static void clear_container(GtkContainer *box) {
    GList *children = gtk_container_get_children(box);
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

/* UI refresh                                                                */

/** Rebuild the requested portion(s) of the game screen on the GTK thread. */
static gboolean do_refresh(gpointer data) {
    RefreshKind kind = (RefreshKind)GPOINTER_TO_INT(data);

    if (kind == REFRESH_COMMUNITY || kind == REFRESH_ALL) {
        clear_container(GTK_CONTAINER(g_community_box));
        for (int i = 0; i < COMMUNITY_SIZE; i++) {
            Card c = (i < g_local_table.community_count)
                     ? g_local_table.community[i]
                     : (Card){ -1, -1, 0 };
            gtk_box_pack_start(GTK_BOX(g_community_box),
                               make_card_widget(&c, TRUE), FALSE, FALSE, 4);
        }
        gtk_widget_show_all(g_community_box);
    }

    if (kind == REFRESH_HAND || kind == REFRESH_ALL) {
        clear_container(GTK_CONTAINER(g_hand_box));
        for (int i = 0; i < HAND_SIZE; i++) {
            gtk_box_pack_start(GTK_BOX(g_hand_box),
                               make_card_widget(&g_my_hand[i], FALSE), FALSE, FALSE, 4);
        }
        gtk_widget_show_all(g_hand_box);

        gtk_label_set_text(GTK_LABEL(g_wild_label),
            (g_my_wild.rank >= 0) ? "You hold the Anteater Wild Card!" : "");
    }

    if (kind == REFRESH_STATUS || kind == REFRESH_ALL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Your Points: %d", g_my_points);
        gtk_label_set_text(GTK_LABEL(g_points_label), buf);
        snprintf(buf, sizeof(buf), "Pot: %d", g_pot);
        gtk_label_set_text(GTK_LABEL(g_pot_label), buf);

        gboolean my_turn = g_is_my_turn ? TRUE : FALSE;
        gtk_label_set_text(GTK_LABEL(g_turn_label),
                           my_turn ? "YOUR TURN" : "Waiting...");
        gtk_widget_set_sensitive(g_btn_call,  my_turn);
        gtk_widget_set_sensitive(g_btn_raise, my_turn);
        gtk_widget_set_sensitive(g_btn_fold,  my_turn);
        gtk_widget_set_sensitive(g_btn_check, my_turn);
        gtk_widget_set_sensitive(g_btn_allin, my_turn);
        if (my_turn && g_my_points > 0) {
            gtk_spin_button_set_range(GTK_SPIN_BUTTON(g_raise_spin), 10, g_my_points);
        }
    }

    if (kind == REFRESH_PLAYERS || kind == REFRESH_ALL) {
        clear_container(GTK_CONTAINER(g_player_grid));
        int row = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (g_seat_names[i][0] == '\0' || strcmp(g_seat_names[i], "-") == 0) {
                continue;
            }
            char seat_text[16];
            snprintf(seat_text, sizeof(seat_text), "Seat %d", i);
            char pts_text[32];
            snprintf(pts_text, sizeof(pts_text), "%d pts", g_player_points[i]);

            GtkWidget *seat_lbl = gtk_label_new(seat_text);
            GtkWidget *name_lbl = gtk_label_new(g_seat_names[i]);
            GtkWidget *pts_lbl  = gtk_label_new(pts_text);
            add_class(name_lbl, (i == g_my_seat) ? "player-active" : "player-row");
            add_class(seat_lbl, "player-row");
            add_class(pts_lbl,  "player-row");

            gtk_grid_attach(GTK_GRID(g_player_grid), seat_lbl, 0, row, 1, 1);
            gtk_grid_attach(GTK_GRID(g_player_grid), name_lbl, 1, row, 1, 1);
            gtk_grid_attach(GTK_GRID(g_player_grid), pts_lbl,  2, row, 1, 1);
            row++;
        }
        gtk_widget_show_all(g_player_grid);
    }

    return G_SOURCE_REMOVE;
}

/** Queue a UI refresh of the given kind from any thread. */
static void request_refresh(RefreshKind kind) {
    g_idle_add(do_refresh, GINT_TO_POINTER(kind));
}

/* Showdown / result dialog                                                  */

typedef struct {
    int  winner_seat;
    char hand_name[64];
    int  bonus;
} ResultInfo;

/** Modal prompt offering the winner the option to reveal their hand. */
static gboolean show_result_dialog(gpointer data) {
    ResultInfo *info = (ResultInfo *)data;

    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "Would you like to reveal your hand?\n\nYour best hand: %s%s",
        info->hand_name,
        info->bonus ? "\n(Anteater Wild Card Bonus!)" : "");
    gtk_window_set_title(GTK_WINDOW(dialog), "Show Hand?");

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES) {
        char line[128];
        snprintf(line, sizeof(line), "You revealed your hand: %s", info->hand_name);
        chat_post(line);
    }
    gtk_widget_destroy(dialog);
    free(info);
    return G_SOURCE_REMOVE;
}

/* Protocol parsing                                                          */

/** Safe wrapper around strtok: returns "" instead of NULL for missing fields. */
static const char *next_field(char **save) {
    char *tok = strtok_r(NULL, "|", save);
    return tok ? tok : "";
}

/** Parse and apply one server message. */
static void handle_message(const char *line) {
    char buf[MAX_MSG_LEN];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *save = NULL;
    char *type = strtok_r(buf, "|", &save);
    if (type == NULL) return;

    if (strcmp(type, MSG_WELCOME) == 0) {
        g_my_seat   = atoi(next_field(&save));
        g_my_points = atoi(next_field(&save));
        request_refresh(REFRESH_STATUS);

    } else if (strcmp(type, MSG_SEATS) == 0) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const char *name = next_field(&save);
            strncpy(g_seat_names[i], name[0] ? name : "-", MAX_NAME_LEN - 1);
            g_seat_names[i][MAX_NAME_LEN - 1] = '\0';
        }
        request_refresh(REFRESH_PLAYERS);

    } else if (strcmp(type, MSG_DEAL) == 0) {
        int r0 = atoi(next_field(&save)), s0 = atoi(next_field(&save));
        int r1 = atoi(next_field(&save)), s1 = atoi(next_field(&save));
        int wr = atoi(next_field(&save)), ws = atoi(next_field(&save));
        g_my_hand[0] = (Card){ (int8_t)r0, (int8_t)s0, 0 };
        g_my_hand[1] = (Card){ (int8_t)r1, (int8_t)s1, 0 };
        g_my_wild    = (wr >= 0) ? (Card){ (int8_t)wr, (int8_t)ws, 1 }
                                 : (Card){ -1, -1, 0 };
        g_local_table.community_count = 0;
        g_is_my_turn  = 0;
        g_current_bet = 0;
        g_pot         = 0;
        request_refresh(REFRESH_ALL);

    } else if (strcmp(type, MSG_COMMUNITY) == 0) {
        g_local_table.community_count = 0;
        const char *rt;
        while ((rt = strtok_r(NULL, "|", &save)) != NULL &&
               g_local_table.community_count < COMMUNITY_SIZE) {
            const char *st = next_field(&save);
            g_local_table.community[g_local_table.community_count++] =
                (Card){ (int8_t)atoi(rt), (int8_t)atoi(st), 0 };
        }
        request_refresh(REFRESH_COMMUNITY);

    } else if (strcmp(type, MSG_TURN) == 0) {
        int turn_seat = atoi(next_field(&save));
        g_current_bet = atoi(next_field(&save));
        g_pot         = atoi(next_field(&save));
        g_is_my_turn  = (turn_seat == g_my_seat);
        request_refresh(REFRESH_STATUS);

    } else if (strcmp(type, MSG_POINTS) == 0) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            g_player_points[i] = atoi(next_field(&save));
        }
        g_my_points = g_player_points[(g_my_seat >= 0) ? g_my_seat : 0];
        request_refresh(REFRESH_PLAYERS);
        request_refresh(REFRESH_STATUS);

    } else if (strcmp(type, MSG_POT) == 0) {
        g_pot = atoi(next_field(&save));
        request_refresh(REFRESH_STATUS);

    } else if (strcmp(type, MSG_RESULT) == 0) {
        int   winner = atoi(next_field(&save));
        const char *hand = next_field(&save);
        (void)next_field(&save); /* pot field (display uses cached g_pot) */
        int   bonus  = atoi(next_field(&save));

        char msg[256];
        if (winner == WINNER_TIE) {
            snprintf(msg, sizeof(msg), "It's a tie! Pot split.");
        } else if (winner == g_my_seat) {
            snprintf(msg, sizeof(msg), "YOU WIN with %s! +%d pts%s",
                     hand[0] ? hand : "?", g_pot,
                     bonus ? " + WILD BONUS!" : "");
        } else {
            const char *wname = (winner >= 0 && winner < MAX_PLAYERS)
                                ? g_seat_names[winner] : "?";
            snprintf(msg, sizeof(msg), "%s wins with %s%s",
                     wname, hand[0] ? hand : "?",
                     bonus ? " (used Anteater Wild!)" : "");
        }
        gtk_label_set_text(GTK_LABEL(g_status_label), msg);
        chat_post(msg);
        g_is_my_turn = 0;
        request_refresh(REFRESH_STATUS);

        if (winner == g_my_seat) {
            ResultInfo *info = malloc(sizeof(ResultInfo));
            if (info != NULL) {
                info->winner_seat = winner;
                info->bonus = bonus;
                snprintf(info->hand_name, sizeof(info->hand_name),
                         "%s", hand[0] ? hand : "Unknown");
                g_idle_add(show_result_dialog, info);
            }
        }

    } else if (strcmp(type, MSG_INFO) == 0) {
        const char *msg = next_field(&save);
        if (msg[0]) chat_post(msg);

    } else if (strcmp(type, MSG_CHAT_BC) == 0) {
        const char *from = next_field(&save);
        const char *msg  = next_field(&save);
        if (from[0] && msg[0]) {
            char full[MAX_MSG_LEN];
            snprintf(full, sizeof(full), "[%s] %s", from, msg);
            chat_post(full);
        }

    } else if (strcmp(type, MSG_SHOWCARDS) == 0) {
        int seat = atoi(next_field(&save));
        const char *name = next_field(&save);
        int r0 = atoi(next_field(&save)), s0 = atoi(next_field(&save));
        int r1 = atoi(next_field(&save)), s1 = atoi(next_field(&save));
        int is_wild = atoi(next_field(&save));
        if (seat == g_my_seat) return; /* don't echo our own cards */

        char line[160];
        snprintf(line, sizeof(line), "[SHOWDOWN] %s: %s%s + %s%s%s",
                 name, RANK_STR[r0], SUIT_STR[s0], RANK_STR[r1], SUIT_STR[s1],
                 is_wild ? " + WILD" : "");
        chat_post(line);

    } else if (strcmp(type, MSG_ERROR) == 0) {
        const char *msg = next_field(&save);
        if (msg[0]) {
            char full[MAX_MSG_LEN];
            snprintf(full, sizeof(full), "[!] %s", msg);
            gtk_label_set_text(GTK_LABEL(g_status_label), full);
        }
    }
}

/* Network I/O                                                               */

/** Receive loop: split the byte stream into newline-framed messages. */
static void *recv_thread(void *arg) {
    (void)arg;
    char chunk[MAX_MSG_LEN * 4];
    char line[MAX_MSG_LEN];
    int  line_len = 0;

    while (1) {
        ssize_t n = recv(g_server_fd, chunk, sizeof(chunk) - 1, 0);
        if (n <= 0) {
            chat_post("[!] Disconnected from server.");
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                line[line_len] = '\0';
                if (line_len > 0) handle_message(line);
                line_len = 0;
            } else if (line_len < MAX_MSG_LEN - 1) {
                line[line_len++] = chunk[i];
            }
        }
    }
    return NULL;
}

/** Send one newline-terminated, printf-formatted message to the server. */
static void send_to_server(const char *fmt, ...) {
    if (g_server_fd < 0) return;

    char buf[MAX_MSG_LEN];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);

    if (len < 0) return;
    if (len > (int)sizeof(buf) - 2) len = (int)sizeof(buf) - 2;
    buf[len]     = '\n';
    buf[len + 1] = '\0';

    send(g_server_fd, buf, strlen(buf), 0);
}

/* Action callbacks                                                          */

/** Disable local turn state immediately after acting (server confirms later). */
static void end_local_turn(void) {
    g_is_my_turn = 0;
    request_refresh(REFRESH_STATUS);
}

static void on_call(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    send_to_server("%s|CALL|0", MSG_ACTION);
    end_local_turn();
}

static void on_fold(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    send_to_server("%s|FOLD|0", MSG_ACTION);
    end_local_turn();
}

static void on_check(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    send_to_server("%s|CHECK|0", MSG_ACTION);
    end_local_turn();
}

static void on_raise(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    int amount = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_raise_spin));
    send_to_server("%s|RAISE|%d", MSG_ACTION, amount);
    end_local_turn();
}

static void on_allin(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    send_to_server("%s|RAISE|%d", MSG_ACTION, g_my_points);
    end_local_turn();
}

static void on_chat_send(GtkButton *b, gpointer d) {
    (void)b; (void)d;
    const char *msg = gtk_entry_get_text(GTK_ENTRY(g_chat_entry));
    if (msg != NULL && msg[0] != '\0') {
        send_to_server("%s|%s", MSG_CHAT, msg);
        gtk_entry_set_text(GTK_ENTRY(g_chat_entry), "");
    }
}

/* Connection                                                                */

/** Validate inputs, connect to the server, and switch to the game screen. */
static void on_connect(GtkButton *button, gpointer data) {
    (void)button; (void)data;

    const char *name = gtk_entry_get_text(GTK_ENTRY(g_entry_name));
    const char *host = gtk_entry_get_text(GTK_ENTRY(g_entry_host));
    const char *port = gtk_entry_get_text(GTK_ENTRY(g_entry_port));

    if (name == NULL || name[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(g_login_status), "Please enter a username.");
        return;
    }

    strncpy(g_my_name, name, MAX_NAME_LEN - 1);
    g_my_name[MAX_NAME_LEN - 1] = '\0';

    int port_num = (port && port[0]) ? atoi(port) : DEFAULT_PORT;
    const char *host_str = (host && host[0]) ? host : "127.0.0.1";

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        gtk_label_set_text(GTK_LABEL(g_login_status), "Could not create socket.");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port_num);
    if (inet_pton(AF_INET, host_str, &addr.sin_addr) != 1) {
        gtk_label_set_text(GTK_LABEL(g_login_status), "Invalid server IP address.");
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    if (connect(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        gtk_label_set_text(GTK_LABEL(g_login_status),
                           "Connection failed. Is the server running?");
        close(g_server_fd);
        g_server_fd = -1;
        return;
    }

    send_to_server("%s|%s", MSG_JOIN, g_my_name);
    gtk_stack_set_visible_child_name(GTK_STACK(g_main_stack), "game");

    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, NULL) == 0) {
        pthread_detach(tid);
    }

    send_to_server("%s", MSG_READY);
}

/* Screen construction                                                       */

/** Build the login screen. */
static GtkWidget *build_login_screen(void) {
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 40);

    GtkWidget *title = gtk_label_new("Anteater Poker");
    add_class(title, "screen-title");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 8);

    GtkWidget *subtitle = gtk_label_new("UCI Edition — EECS 22L");
    add_class(subtitle, "info-lbl");
    gtk_box_pack_start(GTK_BOX(vbox), subtitle, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(vbox),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 8);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *name_lbl = gtk_label_new("Username:");
    add_class(name_lbl, "info-lbl");
    g_entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_name), "e.g. ZotFan42");
    gtk_grid_attach(GTK_GRID(grid), name_lbl,     0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), g_entry_name, 1, 0, 1, 1);

    GtkWidget *host_lbl = gtk_label_new("Server IP:");
    add_class(host_lbl, "info-lbl");
    g_entry_host = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_host), "127.0.0.1");
    gtk_grid_attach(GTK_GRID(grid), host_lbl,     0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), g_entry_host, 1, 1, 1, 1);

    GtkWidget *port_lbl = gtk_label_new("Port:");
    add_class(port_lbl, "info-lbl");
    g_entry_port = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_entry_port), "9000");
    gtk_grid_attach(GTK_GRID(grid), port_lbl,     0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), g_entry_port, 1, 2, 1, 1);

    GtkWidget *connect_btn = gtk_button_new_with_label("Connect & Join Game");
    g_signal_connect(connect_btn, "clicked", G_CALLBACK(on_connect), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), connect_btn, FALSE, FALSE, 8);

    g_login_status = gtk_label_new("");
    add_class(g_login_status, "info-lbl");
    gtk_box_pack_start(GTK_BOX(vbox), g_login_status, FALSE, FALSE, 0);

    return vbox;
}

/** Build the main game screen (table on the left, players/chat on the right). */
static GtkWidget *build_game_screen(void) {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    /* ---- Left: table ---- */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(left), 10);
    gtk_paned_pack1(GTK_PANED(paned), left, TRUE, FALSE);

    GtkWidget *title = gtk_label_new("Anteater Poker");
    add_class(title, "screen-title");
    gtk_box_pack_start(GTK_BOX(left), title, FALSE, FALSE, 2);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_box_pack_start(GTK_BOX(left), status_bar, FALSE, FALSE, 0);
    g_pot_label    = gtk_label_new("Pot: 0");
    g_points_label = gtk_label_new("Points: 1000");
    g_turn_label   = gtk_label_new("Waiting...");
    add_class(g_pot_label, "info-lbl");
    add_class(g_points_label, "info-lbl");
    add_class(g_turn_label, "my-turn");
    gtk_box_pack_start(GTK_BOX(status_bar), g_pot_label,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(status_bar), g_points_label, FALSE, FALSE, 0);
    gtk_box_pack_end  (GTK_BOX(status_bar), g_turn_label,   FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(left),
        gtk_label_new("Community Cards"), FALSE, FALSE, 0);
    g_community_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(left), g_community_box, FALSE, FALSE, 4);
    for (int i = 0; i < COMMUNITY_SIZE; i++) {
        Card empty = { -1, -1, 0 };
        gtk_box_pack_start(GTK_BOX(g_community_box),
                           make_card_widget(&empty, TRUE), FALSE, FALSE, 4);
    }

    gtk_box_pack_start(GTK_BOX(left),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(left), gtk_label_new("Your Hand"), FALSE, FALSE, 0);
    g_hand_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(left), g_hand_box, FALSE, FALSE, 4);
    for (int i = 0; i < HAND_SIZE; i++) {
        Card empty = { -1, -1, 0 };
        g_my_hand[i] = empty;
        gtk_box_pack_start(GTK_BOX(g_hand_box),
                           make_card_widget(&empty, FALSE), FALSE, FALSE, 4);
    }
    g_wild_label = gtk_label_new("");
    add_class(g_wild_label, "my-turn");
    gtk_box_pack_start(GTK_BOX(left), g_wild_label, FALSE, FALSE, 2);

    gtk_box_pack_start(GTK_BOX(left),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    /* ---- Action buttons ---- */
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(left), actions, FALSE, FALSE, 4);

    g_btn_call  = gtk_button_new_with_label("Call");
    g_btn_check = gtk_button_new_with_label("Check");
    g_btn_raise = gtk_button_new_with_label("Raise");
    g_btn_fold  = gtk_button_new_with_label("Fold");
    g_btn_allin = gtk_button_new_with_label("All In");
    add_class(g_btn_call,  "btn-call");
    add_class(g_btn_check, "btn-check");
    add_class(g_btn_raise, "btn-raise");
    add_class(g_btn_fold,  "btn-fold");
    add_class(g_btn_allin, "btn-raise");

    g_raise_spin = gtk_spin_button_new_with_range(10, 500, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_raise_spin), 50);

    g_signal_connect(g_btn_call,  "clicked", G_CALLBACK(on_call),  NULL);
    g_signal_connect(g_btn_check, "clicked", G_CALLBACK(on_check), NULL);
    g_signal_connect(g_btn_raise, "clicked", G_CALLBACK(on_raise), NULL);
    g_signal_connect(g_btn_fold,  "clicked", G_CALLBACK(on_fold),  NULL);
    g_signal_connect(g_btn_allin, "clicked", G_CALLBACK(on_allin), NULL);

    gtk_widget_set_sensitive(g_btn_call,  FALSE);
    gtk_widget_set_sensitive(g_btn_check, FALSE);
    gtk_widget_set_sensitive(g_btn_raise, FALSE);
    gtk_widget_set_sensitive(g_btn_fold,  FALSE);
    gtk_widget_set_sensitive(g_btn_allin, FALSE);

    gtk_box_pack_start(GTK_BOX(actions), g_btn_call,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), g_btn_check,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), g_btn_raise,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), g_raise_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), g_btn_fold,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(actions), g_btn_allin,  FALSE, FALSE, 0);

    g_status_label = gtk_label_new("Connecting to server...");
    add_class(g_status_label, "info-lbl");
    gtk_box_pack_start(GTK_BOX(left), g_status_label, FALSE, FALSE, 4);

    /* ---- Right: players + chat ---- */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(right), 10);
    gtk_paned_pack2(GTK_PANED(paned), right, FALSE, FALSE);

    gtk_box_pack_start(GTK_BOX(right), gtk_label_new("Players"), FALSE, FALSE, 0);
    g_player_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g_player_grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(g_player_grid), 8);
    gtk_box_pack_start(GTK_BOX(right), g_player_grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(right),
        gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(right), gtk_label_new("Chat"), FALSE, FALSE, 0);
    GtkWidget *chat_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(chat_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(chat_view), GTK_WRAP_WORD);
    g_chat_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(chat_view));
    GtkWidget *chat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(chat_scroll), 200);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(chat_scroll), 220);
    gtk_container_add(GTK_CONTAINER(chat_scroll), chat_view);
    gtk_box_pack_start(GTK_BOX(right), chat_scroll, TRUE, TRUE, 0);

    GtkWidget *chat_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(right), chat_row, FALSE, FALSE, 0);
    g_chat_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_chat_entry), "Say something...");
    GtkWidget *send_btn = gtk_button_new_with_label("Send");
    g_signal_connect(send_btn,     "clicked",  G_CALLBACK(on_chat_send), NULL);
    g_signal_connect(g_chat_entry, "activate", G_CALLBACK(on_chat_send), NULL);
    gtk_box_pack_start(GTK_BOX(chat_row), g_chat_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(chat_row), send_btn,     FALSE, FALSE, 0);

    return paned;
}

/* Application lifecycle                                                     */

static void on_activate(GtkApplication *app, gpointer data) {
    (void)data;
    apply_css();

    memset(&g_local_table, 0, sizeof(g_local_table));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        g_player_points[i] = 0;
        g_seat_names[i][0] = '\0';
    }
    g_my_hand[0] = (Card){ -1, -1, 0 };
    g_my_hand[1] = (Card){ -1, -1, 0 };
    g_my_wild    = (Card){ -1, -1, 0 };

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Anteater Poker — Client");
    gtk_window_set_default_size(GTK_WINDOW(win), 920, 600);

    g_main_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(g_main_stack),
        GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT);
    gtk_container_add(GTK_CONTAINER(win), g_main_stack);

    gtk_stack_add_named(GTK_STACK(g_main_stack), build_login_screen(), "login");
    gtk_stack_add_named(GTK_STACK(g_main_stack), build_game_screen(),  "game");
    gtk_stack_set_visible_child_name(GTK_STACK(g_main_stack), "login");

    gtk_widget_show_all(win);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(
        "edu.uci.eecs22l.poker_client", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    if (g_server_fd >= 0) {
        send_to_server("%s", MSG_LEAVE);
        close(g_server_fd);
    }
    return status;
}
