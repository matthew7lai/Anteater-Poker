/*
 * types.h  —  Anteater Poker  (Team 23, EECS 22L)
 * Shared data structures and protocol constants used by both
 * poker_server and poker_client.
 */

#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>

/* ── Sizes ─────────────────────────────────────────── */
#define MAX_PLAYERS       6
#define MAX_NAME_LEN      32
#define MAX_MSG_LEN       512
#define DECK_SIZE         53   /* 52 standard + 1 Anteater Wild Card */
#define COMMUNITY_SIZE    5
#define HAND_SIZE         2
#define DEFAULT_PORT      9000
#define DEFAULT_POINTS    1000
#define WILDCARD_BONUS    200   /* bonus pts for winning with wild card */

/* ── Card ───────────────────────────────────────────── */
/* ranks 0-12 = 2..A;  suit 0-3 = Clubs/Diamonds/Hearts/Spades
   rank=13 suit=4 = Anteater Wild Card                         */
typedef struct {
    int8_t rank;    /* 0..12 or 13 for wild */
    int8_t suit;    /* 0..3  or  4 for wild */
    int8_t is_wild;
} Card;

/* ── Player (shared view – safe to send over wire) ─── */
typedef struct {
    char   name[MAX_NAME_LEN];
    int    points;
    int    seat;          /* 0-based, -1 = not seated */
    int    current_bet;
    int    folded;
    int    active;        /* connected & in game      */
    int    used_wild;     /* used anteater card in hand */
    Card   hand[HAND_SIZE];
    Card   wild_card;     /* the dealt Anteater Wild Card (if any) */
} Player;

/* ── Table (shared game state) ──────────────────────── */
typedef struct {
    Player  players[MAX_PLAYERS];
    int     num_players;
    Card    community[COMMUNITY_SIZE];
    int     community_count; /* how many community cards revealed so far */
    int     pot;
    int     current_bet;
    int     dealer_seat;
    int     current_turn;    /* seat index of player whose turn it is */
    int     round;           /* 0=pre-flop 1=flop 2=turn 3=river 4=showdown */
    int     game_started;
    int     hand_over;
    int     winner_seat;     /* -1 = none yet, -2 = tie */
    char    winner_hand_name[32];
} Table;

/* ══════════════════════════════════════════════════════
   Network Protocol  –  all messages are plain text lines
   Format:  MSG_TYPE|field1|field2|...\n
   ══════════════════════════════════════════════════════ */

/* Client → Server */
#define MSG_JOIN       "JOIN"        /* JOIN|username              */
#define MSG_SEAT       "SEAT"        /* SEAT|seat_number           */
#define MSG_READY      "READY"       /* READY                      */
#define MSG_ACTION     "ACTION"      /* ACTION|CALL/RAISE/FOLD/CHECK|amount */
#define MSG_WILDUSE    "WILDUSE"     /* WILDUSE|rank|suit          */
#define MSG_CHAT       "CHAT"        /* CHAT|message               */
#define MSG_LEAVE      "LEAVE"       /* LEAVE                      */

/* Server → Client */
#define MSG_WELCOME    "WELCOME"     /* WELCOME|seat|points        */
#define MSG_SEATS      "SEATS"       /* SEATS|seat0_name|...|seat5_name */
#define MSG_DEAL       "DEAL"        /* DEAL|r0|s0|r1|s1|wr|ws    (hand+wild) */
#define MSG_COMMUNITY  "COMMUNITY"   /* COMMUNITY|r0|s0|r1|s1|... */
#define MSG_TURN       "TURN"        /* TURN|seat|current_bet|pot  */
#define MSG_POT        "POT"         /* POT|amount                 */
#define MSG_RESULT     "RESULT"      /* RESULT|winner_seat|hand_name|pot|wild_bonus */
#define MSG_POINTS     "POINTS"      /* POINTS|seat0_pts|...|seat5_pts */
#define MSG_CHAT_BC    "CHATBC"      /* CHATBC|from_name|message   */
#define MSG_ERROR      "ERROR"       /* ERROR|message              */
#define MSG_INFO       "INFO"        /* INFO|message               */
#define MSG_GAMESTATE  "GAMESTATE"   /* full table state (JSON-like) */

/* Hand rank names (evaluation) */
static const char *HAND_RANK_NAMES[] = {
    "High Card", "One Pair", "Two Pair", "Three of a Kind",
    "Straight", "Flush", "Full House", "Four of a Kind",
    "Straight Flush", "Royal Flush"
};

/* Rank / suit display helpers */
static const char *RANK_STR[] = {
    "2","3","4","5","6","7","8","9","10","J","Q","K","A","🐜"
};
static const char *SUIT_STR[] = { "♣","♦","♥","♠","*" };

#endif /* TYPES_H */
